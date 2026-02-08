#include "Renderer.h"
#include <cassert>
#include <windows.h>
#include <filesystem>
#include <string>
#include <wincodec.h>
#include <windowsx.h>
#include <vector>
#include <wrl.h>
#include <cmath>

#include "Foundation/Math/MathUtil.h"
#include "d3dx12.h"

using namespace std;

namespace {
    using Microsoft::WRL::ComPtr;

    static std::filesystem::path GetExecutableDir()
    {
        wchar_t exePath[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len == MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exePath).parent_path();
    }

    static std::filesystem::path ResolveAssetPath(const std::wstring& relative)
    {
        const std::filesystem::path exeDir = GetExecutableDir();
        const std::filesystem::path candidates[] = {
            exeDir / L"..\\..\\Assets" / relative,
            std::filesystem::current_path() / L"Assets" / relative,
            std::filesystem::current_path() / L"..\\Assets" / relative,
            std::filesystem::current_path() / L"..\\..\\Assets" / relative,
        };
        for (const auto& path : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                return path;
            }
        }
        return candidates[0];
    }

    // Load image via WIC and convert to RGBA8 in CPU memory
    static bool LoadRgba8ViaWIC(const wchar_t* path, std::vector<uint8_t>& pixels, UINT& width, UINT& height)
    {
        pixels.clear(); width = height = 0;

        ComPtr<IWICImagingFactory> wicFactory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr)) return false;

        hr = wicFactory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr)) return false;

        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) return false;

        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return false;

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return false;

        UINT w = 0, h = 0;
        hr = frame->GetSize(&w, &h);
        if (FAILED(hr) || w == 0 || h == 0) return false;

        pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(hr)) { pixels.clear(); return false; }

        width = w; height = h;
        return true;
    }

    struct CameraCBData
    {
        float mvp[16];
        float world[16];
    };

    struct PointLightGPU
    {
        float posRange[4];
        float colorIntensity[4];
    };

    struct SpotLightGPU
    {
        float posRange[4];
        float dirCosInner[4];
        float colorIntensity[4];
        float params[4]; // x: cosOuter
    };

    struct LightCBData
    {
        float lightVP[16];
        float dirDir[4];
        float dirColor[4];
        float lightCounts[4]; // x: pointCount, y: spotCount
    };

    static bool CreateUploadBuffer(SasamiRenderer::IRHIDevice& device,
                                   UINT64 size,
                                   SasamiRenderer::Resource& outResource,
                                   void** outMappedPtr)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resDesc = {};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width = size;
        resDesc.Height = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.SampleDesc.Count = 1;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = device.CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    outResource);
        if (FAILED(hr)) {
            return false;
        }

        if (outMappedPtr) {
            hr = outResource->Map(0, nullptr, outMappedPtr);
            if (FAILED(hr)) {
                outResource.Reset();
                *outMappedPtr = nullptr;
                return false;
            }
        }

        return true;
    }
}

namespace SasamiRenderer
{
    using Math::Mul4x4;

    Renderer::~Renderer()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }

        for (auto& frame : m_frames) {
            if (frame.cameraCB.IsValid() && frame.cameraCBPtr) {
                frame.cameraCB->Unmap(0, nullptr);
                frame.cameraCBPtr = nullptr;
            }
            if (frame.lightCB.IsValid() && frame.lightCBPtr) {
                frame.lightCB->Unmap(0, nullptr);
                frame.lightCBPtr = nullptr;
            }
            if (frame.pointLightBuffer.IsValid() && frame.pointLightBufferPtr) {
                frame.pointLightBuffer->Unmap(0, nullptr);
                frame.pointLightBufferPtr = nullptr;
            }
            if (frame.spotLightBuffer.IsValid() && frame.spotLightBufferPtr) {
                frame.spotLightBuffer->Unmap(0, nullptr);
                frame.spotLightBufferPtr = nullptr;
            }
        }

        if (m_frameFenceEvent) {
            CloseHandle(m_frameFenceEvent);
            m_frameFenceEvent = nullptr;
        }

        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
    }

    bool Renderer::Initialize(HWND hWnd, UINT width, UINT height)
    {
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(coHr)) {
            m_comInitialized = true;
        } else if (coHr != RPC_E_CHANGED_MODE) {
            return false;
        }

        RECT rc{};
        GetClientRect(hWnd, &rc);
        UINT clientW = static_cast<UINT>(rc.right - rc.left);
        UINT clientH = static_cast<UINT>(rc.bottom - rc.top);
        if (clientW == 0 || clientH == 0) {
            clientW = width;
            clientH = height;
        }

        if (!IsGraphicsRuntimeEnabled(m_graphicsRuntime)) {
            std::string msg = "Selected graphics runtime is disabled by build symbol: ";
            msg += GraphicsRuntimeToString(m_graphicsRuntime);
            msg += "\n";
            OutputDebugStringA(msg.c_str());
            return false;
        }

        m_device = CreateRHIDevice(m_graphicsRuntime);
        if (!m_device) {
            std::string msg = "Failed to create graphics runtime: ";
            msg += GraphicsRuntimeToString(m_graphicsRuntime);
            msg += "\n";
            OutputDebugStringA(msg.c_str());
            return false;
        }
        if (!m_device->Initialize(hWnd, clientW, clientH)) { return false; }
        if (!m_rtBinder.Initialize(*m_device, m_device->GetSwapChain(), GetBackBufferCount())) { return false; }
        if (!m_rlConfig.Initialize(*m_device)) { return false; }

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 512;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = m_device->CreateDescriptorHeap(srvHeapDesc, m_srvHeap);
        if (FAILED(hr)) { return false; }
        m_srvCapacity = srvHeapDesc.NumDescriptors;
        m_srvNext = 0;

        m_viewport = {0.0f, 0.0f, static_cast<float>(clientW), static_cast<float>(clientH), 0.0f, 1.0f};
        m_scissorRect = {0, 0, static_cast<LONG>(clientW), static_cast<LONG>(clientH)};

        CpuDescriptorHandle iconCpu{};
        GpuDescriptorHandle iconGpu{};
        if (!AllocateSrvRange(1, iconCpu, iconGpu)) {
            return false;
        }
        m_iconSrvCpu = iconCpu;
        m_iconTex.srv = iconGpu;

        CpuDescriptorHandle shadowCpu{};
        GpuDescriptorHandle shadowGpu{};
        if (!AllocateSrvRange(1, shadowCpu, shadowGpu)) {
            return false;
        }

        // Create shadow map (1024x1024, D32)
        {
            D3D12_RESOURCE_DESC smDesc = {};
            smDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            smDesc.Width = m_shadowMapSize;
            smDesc.Height = m_shadowMapSize;
            smDesc.DepthOrArraySize = 1;
            smDesc.MipLevels = 1;
            smDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            smDesc.SampleDesc.Count = 1;
            smDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            smDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear = {};
            clear.Format = DXGI_FORMAT_D32_FLOAT;
            clear.DepthStencil.Depth = 1.0f;
            clear.DepthStencil.Stencil = 0;

            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &smDesc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   &clear, m_shadowMap);
            if (FAILED(hr)) {
                return false;
            }

            D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
            dsvDesc.NumDescriptors = 1;
            dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeapShadow);
            if (FAILED(hr)) {
                return false;
            }

            D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            m_device->CreateDepthStencilView(m_shadowMap, &dsv, m_dsvHeapShadow->GetCPUDescriptorHandleForHeapStart());

            D3D12_SHADER_RESOURCE_VIEW_DESC srvSM = {};
            srvSM.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvSM.Format = DXGI_FORMAT_R32_FLOAT;
            srvSM.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvSM.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(m_shadowMap, &srvSM, shadowCpu);
            m_shadowSrv = shadowGpu;

            m_shadowViewport = {0.0f, 0.0f, static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize), 0.0f, 1.0f};
            m_shadowScissor = {0, 0, static_cast<LONG>(m_shadowMapSize), static_cast<LONG>(m_shadowMapSize)};
        }

        if (!InitializeFrameContexts(GetBackBufferCount())) {
            return false;
        }

        for (auto& frame : m_frames) {
            EnsureLightBuffers(frame, m_pointLights.size(), m_spotLights.size());
        }

        // Create main depth buffer (matches backbuffer size)
        {
            D3D12_RESOURCE_DESC depthDesc = {};
            depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depthDesc.Width = clientW;
            depthDesc.Height = clientH;
            depthDesc.DepthOrArraySize = 1;
            depthDesc.MipLevels = 1;
            depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
            depthDesc.SampleDesc.Count = 1;
            depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear = {};
            clear.Format = DXGI_FORMAT_D32_FLOAT;
            clear.DepthStencil.Depth = 1.0f;
            clear.DepthStencil.Stencil = 0;

            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                   &clear, m_depth);
            if (FAILED(hr)) {
                return false;
            }

            D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
            dsvDesc.NumDescriptors = 1;
            dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeap);
            if (FAILED(hr)) {
                return false;
            }

            D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            m_device->CreateDepthStencilView(m_depth, &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
        }

        hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         m_frames[0].cmdAllocator, &m_rlConfig.GetPipelineState(),
                                         m_mainCommandList);
        if (FAILED(hr)) {
            return false;
        }
        m_mainCommandList.Close();
        m_mainCommandListReady = true;

        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, m_frameFence.GetAddressOf());
        if (FAILED(hr)) {
            return false;
        }

        m_frameFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_frameFenceEvent) {
            return false;
        }

        return true;
    }

    bool Renderer::InitializeFrameContexts(UINT frameCount)
    {
        m_frames.clear();
        m_frames.resize(frameCount);

        const UINT cameraCbSize = (sizeof(CameraCBData) + 255u) & ~255u;
        const UINT lightCbSize = (sizeof(LightCBData) + 255u) & ~255u;
        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for (UINT i = 0; i < frameCount; ++i) {
            auto& frame = m_frames[i];

            HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, frame.cmdAllocator);
            if (FAILED(hr)) {
                return false;
            }

            if (!CreateUploadBuffer(*m_device, cameraCbSize, frame.cameraCB, &frame.cameraCBPtr)) {
                return false;
            }
            if (!CreateUploadBuffer(*m_device, lightCbSize, frame.lightCB, &frame.lightCBPtr)) {
                return false;
            }

            CpuDescriptorHandle lightSrvCpu{};
            GpuDescriptorHandle lightSrvGpu{};
            if (!AllocateSrvRange(2, lightSrvCpu, lightSrvGpu)) {
                return false;
            }
            frame.pointSrvCpu = lightSrvCpu;
            frame.spotSrvCpu = { lightSrvCpu.ptr + static_cast<SIZE_T>(inc) };
            frame.lightSrvTable = lightSrvGpu;
            frame.fenceValue = 0;
        }

        return true;
    }

    bool Renderer::ResetCommandListForFrame(UINT frameIndex, CommandList*& outCmdList)
    {
        outCmdList = nullptr;
        if (frameIndex >= m_frames.size() || !m_mainCommandListReady) {
            return false;
        }

        WaitForFrameFence(frameIndex);

        HRESULT hr = m_frames[frameIndex].cmdAllocator.Reset();
        if (FAILED(hr)) {
            OutputDebugStringA("Failed to reset command allocator\n");
            return false;
        }

        hr = m_mainCommandList.Reset(m_frames[frameIndex].cmdAllocator, &m_rlConfig.GetPipelineState());
        if (FAILED(hr)) {
            OutputDebugStringA("Failed to reset command list\n");
            return false;
        }

        outCmdList = &m_mainCommandList;
        return true;
    }

    void Renderer::WaitForFrameFence(UINT frameIndex)
    {
        if (!m_frameFence || frameIndex >= m_frames.size()) {
            return;
        }

        const UINT64 fenceValue = m_frames[frameIndex].fenceValue;
        if (fenceValue == 0) {
            return;
        }

        if (m_frameFence->GetCompletedValue() < fenceValue) {
            m_frameFence->SetEventOnCompletion(fenceValue, m_frameFenceEvent);
            WaitForSingleObject(m_frameFenceEvent, INFINITE);
        }
    }

    void Renderer::SignalFrameFence(UINT frameIndex)
    {
        if (!m_frameFence || frameIndex >= m_frames.size()) {
            return;
        }

        const UINT64 fenceValue = m_nextFenceValue++;
        HRESULT hr = m_device->GetCommandQueue().Signal(m_frameFence.Get(), fenceValue);
        if (SUCCEEDED(hr)) {
            m_frames[frameIndex].fenceValue = fenceValue;
        }
    }

    bool Renderer::AllocateSrvRange(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)
    {
        if (!m_device || !m_srvHeap.Get() || count == 0) {
            return false;
        }
        if (m_srvNext + count > m_srvCapacity) {
            OutputDebugStringA("SRV heap exhausted\n");
            return false;
        }

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto gpuStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        outCpu.ptr = cpuStart.ptr + static_cast<SIZE_T>(m_srvNext) * inc;
        outGpu.ptr = gpuStart.ptr + static_cast<SIZE_T>(m_srvNext) * inc;
        m_srvNext += count;
        return true;
    }

    void Renderer::Render(const OverlayRenderCallback& overlay)
    {
        if (!m_device || m_frames.empty()) {
            return;
        }

        const UINT backIndex = m_device->GetSwapChain()->GetCurrentBackBufferIndex();
        if (backIndex >= m_frames.size()) {
            return;
        }

        CommandList* cmdList = nullptr;
        if (!ResetCommandListForFrame(backIndex, cmdList)) {
            return;
        }
        auto& frame = m_frames[backIndex];

        EnsureIconTextureUploaded(cmdList);
        ShadowPass(cmdList, frame);
        TransitionBackBufferToRenderTarget(cmdList, backIndex);
        ClearAndBindMainTargets(cmdList, backIndex);
        MainPass(cmdList, frame);

        auto rtvHandle = m_rtBinder.GetRTV(static_cast<int>(backIndex));
        if (overlay) {
            overlay(*cmdList, rtvHandle);
        }

        TransitionBackBufferToPresent(cmdList, backIndex);
        SubmitAndPresent(cmdList, backIndex);
    }

    void Renderer::ShadowPass(CommandList* cmdList, FrameContext& frame)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &barrier);

        cmdList->SetGraphicsRootSignature(m_rlConfig.GetRootSignature());
        if (m_useTessellation) {
            cmdList->SetPipelineState(m_rlConfig.GetTessellationShadowPipelineState());
        } else {
            cmdList->SetPipelineState(m_rlConfig.GetShadowPipelineState());
        }

        DescriptorHeap* heaps[] = { &m_srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(0, m_iconTex.srv);
        cmdList->SetGraphicsRootDescriptorTable(1, m_shadowSrv);
        cmdList->SetGraphicsRootDescriptorTable(4, frame.lightSrvTable);
        cmdList->RSSetViewports(1, &m_shadowViewport);
        cmdList->RSSetScissorRects(1, &m_shadowScissor);
        cmdList->IASetPrimitiveTopology(m_useTessellation ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                          : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto dsv = m_dsvHeapShadow->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        float yaw = m_lightYaw, pitch = m_lightPitch;
        float cy = cos(yaw), sy = sin(yaw);
        float cp = cos(pitch), sp = sin(pitch);
        float world[16] = {
            cy,      sp * sy,   -cp * sy,  0,
            0,       cp,         sp,       0,
            sy,     -sp * cy,    cp * cy,  0,
            0,       0,          0,        1,
        };
        float view[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,m_lightDistance,1 };
        float l = -m_lightOrthoHalf, r = m_lightOrthoHalf;
        float b = -m_lightOrthoHalf, t = m_lightOrthoHalf;
        float n = m_lightNear, f = m_lightFar;
        float proj[16] = {
            2/(r-l), 0,       0,        0,
            0,       2/(t-b), 0,        0,
            0,       0,      1/(f-n),   0,
            -(r+l)/(r-l), -(t+b)/(t-b), -n/(f-n), 1,
        };
        float wv[16];
        Mul4x4(world, view, wv);
        float lightVP[16];
        Mul4x4(wv, proj, lightVP);
        float fwd[3] = { -sy, sp * cy, -cp * cy };
        float fl = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
        if (fl > 0) { fwd[0] /= fl; fwd[1] /= fl; fwd[2] /= fl; }

        EnsureLightBuffers(frame, m_pointLights.size(), m_spotLights.size());

        if (frame.lightCBPtr) {
            size_t pointCount = m_pointLights.size();
            size_t spotCount = m_spotLights.size();

            if (!frame.pointLightBufferPtr) {
                pointCount = 0;
            } else {
                auto* pointDst = reinterpret_cast<PointLightGPU*>(frame.pointLightBufferPtr);
                for (size_t i = 0; i < pointCount; ++i) {
                    const auto& pl = m_pointLights[i];
                    pointDst[i].posRange[0] = pl.pos[0];
                    pointDst[i].posRange[1] = pl.pos[1];
                    pointDst[i].posRange[2] = pl.pos[2];
                    pointDst[i].posRange[3] = pl.range;
                    pointDst[i].colorIntensity[0] = pl.color[0];
                    pointDst[i].colorIntensity[1] = pl.color[1];
                    pointDst[i].colorIntensity[2] = pl.color[2];
                    pointDst[i].colorIntensity[3] = pl.intensity;
                }
            }

            if (!frame.spotLightBufferPtr) {
                spotCount = 0;
            } else {
                auto* spotDst = reinterpret_cast<SpotLightGPU*>(frame.spotLightBufferPtr);
                for (size_t i = 0; i < spotCount; ++i) {
                    const auto& sl = m_spotLights[i];
                    float scy = cos(sl.yaw), ssy = sin(sl.yaw);
                    float scp = cos(sl.pitch), ssp = sin(sl.pitch);
                    float spotDir[3] = { -ssy, ssp * scy, -scp * scy };
                    float slen = std::sqrt(spotDir[0]*spotDir[0] + spotDir[1]*spotDir[1] + spotDir[2]*spotDir[2]);
                    if (slen > 0) { spotDir[0] /= slen; spotDir[1] /= slen; spotDir[2] /= slen; }
                    float inner = sl.innerAngle;
                    float outer = sl.outerAngle;
                    if (inner > outer) inner = outer;
                    float cosInner = cos(inner);
                    float cosOuter = cos(outer);

                    spotDst[i].posRange[0] = sl.pos[0];
                    spotDst[i].posRange[1] = sl.pos[1];
                    spotDst[i].posRange[2] = sl.pos[2];
                    spotDst[i].posRange[3] = sl.range;
                    spotDst[i].dirCosInner[0] = spotDir[0];
                    spotDst[i].dirCosInner[1] = spotDir[1];
                    spotDst[i].dirCosInner[2] = spotDir[2];
                    spotDst[i].dirCosInner[3] = cosInner;
                    spotDst[i].colorIntensity[0] = sl.color[0];
                    spotDst[i].colorIntensity[1] = sl.color[1];
                    spotDst[i].colorIntensity[2] = sl.color[2];
                    spotDst[i].colorIntensity[3] = sl.intensity;
                    spotDst[i].params[0] = cosOuter;
                    spotDst[i].params[1] = 0.0f;
                    spotDst[i].params[2] = 0.0f;
                    spotDst[i].params[3] = 0.0f;
                }
            }

            LightCBData cb = {};
            memcpy(cb.lightVP, lightVP, sizeof(lightVP));
            cb.dirDir[0] = fwd[0];
            cb.dirDir[1] = fwd[1];
            cb.dirDir[2] = fwd[2];
            cb.dirDir[3] = m_dirIntensity;
            cb.dirColor[0] = m_dirColor[0];
            cb.dirColor[1] = m_dirColor[1];
            cb.dirColor[2] = m_dirColor[2];
            cb.dirColor[3] = 1.0f;
            cb.lightCounts[0] = static_cast<float>(pointCount);
            cb.lightCounts[1] = static_cast<float>(spotCount);
            memcpy(frame.lightCBPtr, &cb, sizeof(cb));
        }

        cmdList->SetGraphicsRootConstantBufferView(3, frame.lightCB->GetGPUVirtualAddress());

        for (const auto& item : m_drawItems) {
            float objLightMVP[16];
            Mul4x4(item.model, lightVP, objLightMVP);
            if (frame.cameraCBPtr) {
                CameraCBData cb = {};
                memcpy(cb.mvp, objLightMVP, sizeof(objLightMVP));
                memcpy(cb.world, item.model, sizeof(item.model));
                memcpy(frame.cameraCBPtr, &cb, sizeof(cb));
            }
            cmdList->SetGraphicsRootConstantBufferView(2, frame.cameraCB->GetGPUVirtualAddress());
            m_meshBuffer.Bind(cmdList, item.meshIndex);
            const auto& items = m_meshBuffer.Items();
            if (item.meshIndex < items.size()) {
                const auto& it = items[item.meshIndex];
                if (it.indexCount > 0) cmdList->DrawIndexedInstanced(it.indexCount, 1, 0, 0, 0);
                else if (it.vertexCount > 0) cmdList->DrawInstanced(it.vertexCount, 1, 0, 0);
            }
        }

        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
    }

    void Renderer::SetDirectionalLightAngles(float yaw, float pitch)
    {
        m_lightYaw = yaw;
        m_lightPitch = pitch;
    }

    void Renderer::SetDirectionalLightDistance(float distance)
    {
        m_lightDistance = distance;
    }

    void Renderer::SetDirectionalLightOrtho(float halfExtent, float nearZ, float farZ)
    {
        m_lightOrthoHalf = halfExtent;
        m_lightNear = nearZ;
        m_lightFar = farZ;
    }

    Renderer::DirectionalLightSettings Renderer::GetDirectionalLightSettings() const
    {
        DirectionalLightSettings settings{};
        settings.yaw = m_lightYaw;
        settings.pitch = m_lightPitch;
        settings.distance = m_lightDistance;
        settings.orthoHalf = m_lightOrthoHalf;
        settings.nearZ = m_lightNear;
        settings.farZ = m_lightFar;
        settings.color[0] = m_dirColor[0];
        settings.color[1] = m_dirColor[1];
        settings.color[2] = m_dirColor[2];
        settings.intensity = m_dirIntensity;
        return settings;
    }

    void Renderer::SetDirectionalLightSettings(const DirectionalLightSettings& settings)
    {
        m_lightYaw = settings.yaw;
        m_lightPitch = settings.pitch;
        m_lightDistance = settings.distance;
        m_lightOrthoHalf = settings.orthoHalf;
        m_lightNear = settings.nearZ;
        m_lightFar = settings.farZ;
        m_dirColor[0] = settings.color[0];
        m_dirColor[1] = settings.color[1];
        m_dirColor[2] = settings.color[2];
        m_dirIntensity = settings.intensity;
    }

    void Renderer::ResizeViewport(UINT width, UINT height)
    {
        if (width == 0 || height == 0 || !m_device) {
            return;
        }

        m_device->WaitForGPU();
        for (auto& frame : m_frames) {
            frame.fenceValue = 0;
        }

        m_rtBinder.Release();

        HRESULT hr = m_device->GetSwapChain()->ResizeBuffers(GetBackBufferCount(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(hr)) {
            OutputDebugStringA("ResizeBuffers failed\n");
            return;
        }

        if (!m_rtBinder.Initialize(*m_device, m_device->GetSwapChain(), GetBackBufferCount())) {
            OutputDebugStringA("RTV rebinding failed\n");
            return;
        }

        m_viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        m_scissorRect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

        m_depth.Reset();
        m_dsvHeap.Reset();

        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr2 = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                                        D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                        &clear, m_depth);
        if (FAILED(hr2)) {
            OutputDebugStringA("Depth recreation failed\n");
            return;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr2 = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeap);
        if (FAILED(hr2)) {
            OutputDebugStringA("DSV heap recreation failed\n");
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        m_device->CreateDepthStencilView(m_depth, &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    void Renderer::TransitionBackBufferToRenderTarget(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = m_rtBinder.GetBackBufferResource(static_cast<int>(backIndex));
        barrier.Transition.pResource = backBuffer ? backBuffer->Get() : nullptr;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void Renderer::ClearAndBindMainTargets(CommandList* cmdList, UINT backIndex)
    {
        auto rtvHandle = m_rtBinder.GetRTV(static_cast<int>(backIndex));
        auto dsvMain = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvMain);
        const FLOAT clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(dsvMain, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    void Renderer::EnsureIconTextureUploaded(CommandList* cmdList)
    {
        if (m_textureUploaded) return;

        const auto kIconPath = ResolveAssetPath(L"SasamiIcon.png");
        UINT texWidth = 0, texHeight = 0;
        std::vector<uint8_t> pixels;
        if (!LoadRgba8ViaWIC(kIconPath.c_str(), pixels, texWidth, texHeight)) {
            OutputDebugStringA("Failed to load texture with WIC\n");
            return;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Alignment = 0;
        texDesc.Width = texWidth;
        texDesc.Height = texHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = m_device->CreateCommittedResource(&defaultHeapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &texDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       m_texture);
        if (FAILED(hr)) return;

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_texture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        hr = m_device->CreateCommittedResource(&uploadHeapProps,
                                               D3D12_HEAP_FLAG_NONE,
                                               &bufferDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr,
                                               m_textureUpload);
        if (FAILED(hr)) return;

        D3D12_SUBRESOURCE_DATA textureData = {};
        textureData.pData = pixels.data();
        textureData.RowPitch = static_cast<LONG_PTR>(texWidth) * 4;
        textureData.SlicePitch = textureData.RowPitch * texHeight;

        UpdateSubresources(cmdList->Get(), m_texture.Get(), m_textureUpload.Get(), 0, 0, 1, &textureData);
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_texture, &srvDesc, m_iconSrvCpu);

        m_iconTex.resource = m_texture;
        m_iconTex.desc.width = texWidth;
        m_iconTex.desc.height = texHeight;
        m_material.Set(TextureSlot::Albedo, &m_iconTex);
        m_textureUploaded = true;
    }

    Texture* Renderer::CreateTextureFromFile(const std::wstring& path, CommandList* cmdList,
                                             std::vector<Resource>& uploads)
    {
        UINT texWidth = 0, texHeight = 0;
        std::vector<uint8_t> pixels;
        if (!LoadRgba8ViaWIC(path.c_str(), pixels, texWidth, texHeight)) {
            return nullptr;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Alignment = 0;
        texDesc.Width = texWidth;
        texDesc.Height = texHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        Resource texture;
        HRESULT hr = m_device->CreateCommittedResource(&defaultHeapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &texDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       texture);
        if (FAILED(hr)) return nullptr;

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        Resource upload;
        hr = m_device->CreateCommittedResource(&uploadHeapProps,
                                               D3D12_HEAP_FLAG_NONE,
                                               &bufferDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr,
                                               upload);
        if (FAILED(hr)) return nullptr;

        D3D12_SUBRESOURCE_DATA textureData = {};
        textureData.pData = pixels.data();
        textureData.RowPitch = static_cast<LONG_PTR>(texWidth) * 4;
        textureData.SlicePitch = textureData.RowPitch * texHeight;

        UpdateSubresources(cmdList->Get(), texture.Get(), upload.Get(), 0, 0, 1, &textureData);
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);

        CpuDescriptorHandle cpu{};
        GpuDescriptorHandle gpu{};
        if (!AllocateSrvRange(1, cpu, gpu)) {
            return nullptr;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(texture, &srvDesc, cpu);

        auto texObj = std::make_unique<Texture>();
        texObj->resource = texture;
        texObj->srv = gpu;
        texObj->desc.width = texWidth;
        texObj->desc.height = texHeight;
        texObj->desc.mips = 1;
        texObj->desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;

        uploads.push_back(upload);
        m_sceneTextures.push_back(std::move(texObj));
        return m_sceneTextures.back().get();
    }

    void Renderer::EnsureLightBuffers(FrameContext& frame, size_t pointCount, size_t spotCount)
    {
        auto growCapacity = [](UINT current, UINT required) -> UINT {
            UINT cap = current > 0 ? current : 1;
            while (cap < required) cap *= 2;
            return cap;
        };

        const UINT requiredPoint = pointCount > 0 ? static_cast<UINT>(pointCount) : 1u;
        const UINT requiredSpot = spotCount > 0 ? static_cast<UINT>(spotCount) : 1u;

        if (requiredPoint > frame.pointLightCapacity) {
            frame.pointLightCapacity = growCapacity(frame.pointLightCapacity, requiredPoint);
            if (frame.pointLightBuffer.IsValid() && frame.pointLightBufferPtr) {
                frame.pointLightBuffer->Unmap(0, nullptr);
            }
            frame.pointLightBuffer.Reset();
            frame.pointLightBufferPtr = nullptr;

            const UINT64 byteSize = static_cast<UINT64>(frame.pointLightCapacity) * sizeof(PointLightGPU);
            if (!CreateUploadBuffer(*m_device, byteSize, frame.pointLightBuffer, &frame.pointLightBufferPtr)) {
                frame.pointLightCapacity = 0;
            }
        }

        if (requiredSpot > frame.spotLightCapacity) {
            frame.spotLightCapacity = growCapacity(frame.spotLightCapacity, requiredSpot);
            if (frame.spotLightBuffer.IsValid() && frame.spotLightBufferPtr) {
                frame.spotLightBuffer->Unmap(0, nullptr);
            }
            frame.spotLightBuffer.Reset();
            frame.spotLightBufferPtr = nullptr;

            const UINT64 byteSize = static_cast<UINT64>(frame.spotLightCapacity) * sizeof(SpotLightGPU);
            if (!CreateUploadBuffer(*m_device, byteSize, frame.spotLightBuffer, &frame.spotLightBufferPtr)) {
                frame.spotLightCapacity = 0;
            }
        }

        if (frame.pointLightBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = frame.pointLightCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(PointLightGPU);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            m_device->CreateShaderResourceView(frame.pointLightBuffer, &srvDesc, frame.pointSrvCpu);
        }

        if (frame.spotLightBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = frame.spotLightCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(SpotLightGPU);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            m_device->CreateShaderResourceView(frame.spotLightBuffer, &srvDesc, frame.spotSrvCpu);
        }
    }

    void Renderer::MainPass(CommandList* cmdList, FrameContext& frame)
    {
        cmdList->SetGraphicsRootSignature(m_rlConfig.GetRootSignature());
        cmdList->RSSetViewports(1, &m_viewport);
        cmdList->RSSetScissorRects(1, &m_scissorRect);
        if (m_useTessellation) {
            cmdList->SetPipelineState(m_rlConfig.GetTessellationPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        } else {
            cmdList->SetPipelineState(m_rlConfig.GetPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

        DescriptorHeap* heaps[] = { &m_srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);

        cmdList->SetGraphicsRootDescriptorTable(1, m_shadowSrv);
        cmdList->SetGraphicsRootDescriptorTable(4, frame.lightSrvTable);

        cmdList->SetGraphicsRootConstantBufferView(3, frame.lightCB->GetGPUVirtualAddress());
        for (const auto& item : m_drawItems) {
            float objMVP[16];
            Mul4x4(item.model, m_cameraPV, objMVP);
            if (frame.cameraCBPtr) {
                CameraCBData cb = {};
                memcpy(cb.mvp, objMVP, sizeof(objMVP));
                memcpy(cb.world, item.model, sizeof(item.model));
                memcpy(frame.cameraCBPtr, &cb, sizeof(cb));
            }
            cmdList->SetGraphicsRootConstantBufferView(2, frame.cameraCB->GetGPUVirtualAddress());
            if (item.texture) {
                cmdList->SetGraphicsRootDescriptorTable(0, item.texture->srv);
            } else {
                m_material.Bind(cmdList, 0);
            }
            m_meshBuffer.Bind(cmdList, item.meshIndex);
            const auto& items = m_meshBuffer.Items();
            if (item.meshIndex < items.size()) {
                const auto& it = items[item.meshIndex];
                if (it.indexCount > 0) cmdList->DrawIndexedInstanced(it.indexCount, 1, 0, 0, 0);
                else if (it.vertexCount > 0) cmdList->DrawInstanced(it.vertexCount, 1, 0, 0);
            }
        }
    }

    void Renderer::TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = m_rtBinder.GetBackBufferResource(static_cast<int>(backIndex));
        barrier.Transition.pResource = backBuffer ? backBuffer->Get() : nullptr;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void Renderer::SubmitAndPresent(CommandList* cmdList, UINT frameIndex)
    {
        if (FAILED(cmdList->Close())) {
            OutputDebugStringA("Failed to close command list\n");
            return;
        }

        ID3D12CommandList* lists[] = { cmdList->Get() };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, lists);
        (void)m_device->GetSwapChain()->Present(1, 0);
        SignalFrameFence(frameIndex);
    }

    void Renderer::UpdateCameraCB(const RenderCameraProxy* camera)
    {
        if (camera) {
            for (int i = 0; i < 16; ++i) {
                m_cameraPV[i] = camera->viewProjection[i];
            }
        } else {
            for (int i = 0; i < 16; ++i) {
                m_cameraPV[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            }
        }
    }

    Texture* Renderer::ResolveSceneTexture(const std::wstring& path)
    {
        if (path.empty()) {
            return nullptr;
        }

        auto cached = m_textureCache.find(path);
        if (cached != m_textureCache.end()) {
            return cached->second;
        }

        CommandAllocator uploadAlloc;
        CommandList uploadList;
        HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc);
        if (SUCCEEDED(hr)) {
            hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc, nullptr, uploadList);
        }
        if (FAILED(hr)) {
            return nullptr;
        }

        std::vector<Resource> uploads;
        Texture* texture = CreateTextureFromFile(path, &uploadList, uploads);
        if (!texture) {
            return nullptr;
        }

        uploadList->Close();
        ID3D12CommandList* lists[] = { uploadList.Get() };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, lists);
        m_device->WaitForGPU();
        uploads.clear();

        m_textureCache[path] = texture;
        return texture;
    }

    void Renderer::SubmitRenderProxies(std::vector<RenderProxy>&& proxies)
    {
        if (!m_device) return;
        if (proxies.empty()) return;

        for (auto& proxy : proxies) {
            const size_t meshIndex = m_meshes.size();
            m_meshes.push_back(std::move(proxy.mesh));

            DrawItem item;
            item.meshIndex = meshIndex;
            item.texture = ResolveSceneTexture(proxy.texturePath);
            memcpy(item.model, proxy.model, sizeof(item.model));
            m_drawItems.push_back(item);
        }

        m_meshBuffer.Upload(*m_device, m_meshes);
    }

    void Renderer::ClearSubmittedRenderProxies()
    {
        m_drawItems.clear();
        m_meshes.clear();
    }

    void Renderer::ClearRenderObjects()
    {
        ClearSubmittedRenderProxies();
    }
}
