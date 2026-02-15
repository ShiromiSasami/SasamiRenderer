#include "Renderer/Core/Renderer.h"
#include <cassert>
#include <windows.h>
#include <string>
#include <windowsx.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include <utility>

#include "Foundation/Diagnostics/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/RendererMathUtility.h"
#include "d3dx12.h"

using namespace std;

namespace {
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
        float cameraPos[4];   // xyz: camera world position
        float iblParams[4];   // x: IBL enable, y: intensity, z: prefilterMaxMip
        float debugParams[4]; // x: gbuffer debug mode
    };

    struct SkyboxVertex
    {
        float position[3];
    };

    static const SkyboxVertex kSkyboxCubeVertices[] = {
        // +X
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f, -1.0f,  1.0f } }, { { 1.0f,  1.0f,  1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f,  1.0f,  1.0f } }, { { 1.0f,  1.0f, -1.0f } },
        // -X
        { { -1.0f, -1.0f,  1.0f } }, { { -1.0f, -1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { -1.0f,  1.0f, -1.0f } }, { { -1.0f,  1.0f,  1.0f } },
        // +Y
        { { -1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, 1.0f } },
        { { -1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, 1.0f } }, { { -1.0f, 1.0f, 1.0f } },
        // -Y
        { { -1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f, -1.0f } }, { { -1.0f, -1.0f, -1.0f } },
        // +Z
        { { -1.0f, -1.0f, 1.0f } }, { { -1.0f,  1.0f, 1.0f } }, { { 1.0f,  1.0f, 1.0f } },
        { { -1.0f, -1.0f, 1.0f } }, { { 1.0f,  1.0f, 1.0f } }, { { 1.0f, -1.0f, 1.0f } },
        // -Z
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f,  1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } }, { { -1.0f, -1.0f, -1.0f } },
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

    static bool CreateTexture2DFromRgba8(SasamiRenderer::IRHIDevice& device,
                                         SasamiRenderer::CommandList* cmdList,
                                         const uint8_t* pixels,
                                         UINT width,
                                         UINT height,
                                         SasamiRenderer::Resource& outTexture,
                                         SasamiRenderer::Resource& outUpload)
    {
        if (!pixels || width == 0 || height == 0 || !cmdList) {
            return false;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = device.CreateCommittedResource(&defaultHeapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr,
                                                    outTexture);
        if (FAILED(hr)) {
            return false;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        hr = device.CreateCommittedResource(&uploadHeapProps,
                                            D3D12_HEAP_FLAG_NONE,
                                            &bufferDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr,
                                            outUpload);
        if (FAILED(hr)) {
            outTexture.Reset();
            return false;
        }

        D3D12_SUBRESOURCE_DATA textureData = {};
        textureData.pData = pixels;
        textureData.RowPitch = static_cast<LONG_PTR>(width) * 4;
        textureData.SlicePitch = textureData.RowPitch * height;
        UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, 1, &textureData);

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }

    static bool CreateTextureCubeFromRgba8Faces(SasamiRenderer::IRHIDevice& device,
                                                SasamiRenderer::CommandList* cmdList,
                                                const std::vector<std::vector<uint8_t>>& facePixels,
                                                UINT width,
                                                UINT height,
                                                SasamiRenderer::Resource& outTexture,
                                                SasamiRenderer::Resource& outUpload)
    {
        if (!cmdList || facePixels.size() != 6 || width == 0 || height == 0) {
            return false;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 6;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = device.CreateCommittedResource(&defaultHeapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr,
                                                    outTexture);
        if (FAILED(hr)) {
            return false;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, 6);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        hr = device.CreateCommittedResource(&uploadHeapProps,
                                            D3D12_HEAP_FLAG_NONE,
                                            &bufferDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr,
                                            outUpload);
        if (FAILED(hr)) {
            outTexture.Reset();
            return false;
        }

        std::vector<D3D12_SUBRESOURCE_DATA> subresources(6);
        for (int i = 0; i < 6; ++i) {
            subresources[i].pData = facePixels[static_cast<size_t>(i)].data();
            subresources[i].RowPitch = static_cast<LONG_PTR>(width) * 4;
            subresources[i].SlicePitch = subresources[i].RowPitch * height;
        }
        UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, 6, subresources.data());

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }

    static bool CreateTextureCubeFromFloatFacesWithMips(SasamiRenderer::IRHIDevice& device,
                                                        SasamiRenderer::CommandList* cmdList,
                                                        const std::vector<std::vector<float>>& subresourcesRgba,
                                                        UINT baseFaceSize,
                                                        UINT mipLevels,
                                                        SasamiRenderer::Resource& outTexture,
                                                        SasamiRenderer::Resource& outUpload)
    {
        if (!cmdList || baseFaceSize == 0 || mipLevels == 0 || subresourcesRgba.size() != static_cast<size_t>(mipLevels) * 6u) {
            return false;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = baseFaceSize;
        texDesc.Height = baseFaceSize;
        texDesc.DepthOrArraySize = 6;
        texDesc.MipLevels = static_cast<UINT16>(mipLevels);
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = device.CreateCommittedResource(&defaultHeapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr,
                                                    outTexture);
        if (FAILED(hr)) {
            return false;
        }

        const UINT subresourceCount = mipLevels * 6;
        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, subresourceCount);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        hr = device.CreateCommittedResource(&uploadHeapProps,
                                            D3D12_HEAP_FLAG_NONE,
                                            &bufferDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr,
                                            outUpload);
        if (FAILED(hr)) {
            outTexture.Reset();
            return false;
        }

        std::vector<std::vector<uint16_t>> halfData(subresourceCount);
        std::vector<D3D12_SUBRESOURCE_DATA> subresourceDescs(subresourceCount);
        for (UINT face = 0; face < 6; ++face) {
            for (UINT mip = 0; mip < mipLevels; ++mip) {
                const UINT subresourceIndex = mip + face * mipLevels;
                const UINT size = (baseFaceSize >> mip) > 0 ? (baseFaceSize >> mip) : 1u;
                const auto& src = subresourcesRgba[subresourceIndex];
                if (src.size() != static_cast<size_t>(size) * size * 4u) {
                    return false;
                }

                auto& dst = halfData[subresourceIndex];
                dst.resize(src.size());
                for (size_t i = 0; i < src.size(); ++i) {
                    dst[i] = SasamiRenderer::Math::FloatToHalf(src[i]);
                }

                subresourceDescs[subresourceIndex].pData = dst.data();
                subresourceDescs[subresourceIndex].RowPitch = static_cast<LONG_PTR>(size) * 8;
                subresourceDescs[subresourceIndex].SlicePitch = subresourceDescs[subresourceIndex].RowPitch * size;
            }
        }

        UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, subresourceCount, subresourceDescs.data());
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }

    static bool CreateTexture2DFromFloatRgba(SasamiRenderer::IRHIDevice& device,
                                             SasamiRenderer::CommandList* cmdList,
                                             const std::vector<float>& pixels,
                                             UINT width,
                                             UINT height,
                                             SasamiRenderer::Resource& outTexture,
                                             SasamiRenderer::Resource& outUpload)
    {
        if (!cmdList || width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height * 4u) {
            return false;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = device.CreateCommittedResource(&defaultHeapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr,
                                                    outTexture);
        if (FAILED(hr)) {
            return false;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        hr = device.CreateCommittedResource(&uploadHeapProps,
                                            D3D12_HEAP_FLAG_NONE,
                                            &bufferDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr,
                                            outUpload);
        if (FAILED(hr)) {
            outTexture.Reset();
            return false;
        }

        std::vector<uint16_t> halfPixels(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i) {
            halfPixels[i] = SasamiRenderer::Math::FloatToHalf(pixels[i]);
        }

        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = halfPixels.data();
        subresource.RowPitch = static_cast<LONG_PTR>(width) * 8;
        subresource.SlicePitch = subresource.RowPitch * height;
        UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, 1, &subresource);

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
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
                frame.cameraCbCapacity = 0;
                frame.cameraCbCount = 0;
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
        auto failInit = [](const char* message) -> bool {
            DebugLog(message);
            return false;
        };

        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(coHr)) {
            m_comInitialized = true;
        } else if (coHr != RPC_E_CHANGED_MODE) {
            return failInit("Renderer::Initialize: CoInitializeEx failed.\n");
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
            DebugLog(msg.c_str());
            return false;
        }

        m_device = CreateRHIDevice(m_graphicsRuntime);
        if (!m_device) {
            std::string msg = "Failed to create graphics runtime: ";
            msg += GraphicsRuntimeToString(m_graphicsRuntime);
            msg += "\n";
            DebugLog(msg.c_str());
            return failInit("Renderer::Initialize: CreateRHIDevice returned null.\n");
        }
        if (!m_device->Initialize(hWnd, clientW, clientH)) {
            return failInit("Renderer::Initialize: IRHIDevice::Initialize failed.\n");
        }
        if (!m_rtBinder.Initialize(*m_device, m_device->GetSwapChain(), GetBackBufferCount())) {
            return failInit("Renderer::Initialize: RenderTargetResourceBinder::Initialize failed.\n");
        }
        if (!m_rlConfig.Initialize(*m_device)) {
            return failInit("Renderer::Initialize: RenderLayerConfigurator::Initialize failed.\n");
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 512;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = m_device->CreateDescriptorHeap(srvHeapDesc, m_srvHeap);
        if (FAILED(hr)) {
            return failInit("Renderer::Initialize: SRV descriptor heap creation failed.\n");
        }
        m_srvCapacity = srvHeapDesc.NumDescriptors;
        m_srvNext = 0;

        m_viewport = {0.0f, 0.0f, static_cast<float>(clientW), static_cast<float>(clientH), 0.0f, 1.0f};
        m_scissorRect = {0, 0, static_cast<LONG>(clientW), static_cast<LONG>(clientH)};

        CpuDescriptorHandle iconCpu{};
        GpuDescriptorHandle iconGpu{};
        if (!AllocateSrvRange(1, iconCpu, iconGpu)) {
            return failInit("Renderer::Initialize: SRV allocation failed for icon texture.\n");
        }
        m_iconSrvCpu = iconCpu;
        m_iconTex.srv = iconGpu;

        CpuDescriptorHandle shadowCpu{};
        GpuDescriptorHandle shadowGpu{};
        if (!AllocateSrvRange(1, shadowCpu, shadowGpu)) {
            return failInit("Renderer::Initialize: SRV allocation failed for shadow map.\n");
        }

        CpuDescriptorHandle skyboxCpu{};
        GpuDescriptorHandle skyboxGpu{};
        if (!AllocateSrvRange(1, skyboxCpu, skyboxGpu)) {
            return failInit("Renderer::Initialize: SRV allocation failed for skybox.\n");
        }
        m_skyboxSrvCpu = skyboxCpu;
        m_skyboxSrv = skyboxGpu;

        CpuDescriptorHandle iblCpu{};
        GpuDescriptorHandle iblGpu{};
        if (!AllocateSrvRange(3, iblCpu, iblGpu)) {
            return failInit("Renderer::Initialize: SRV allocation failed for IBL textures.\n");
        }
        m_iblSrvCpu = iblCpu;
        m_iblSrv = iblGpu;
        {
            const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            CpuDescriptorHandle preCpu = { iblCpu.ptr + static_cast<SIZE_T>(inc) };
            CpuDescriptorHandle brdfCpu = { iblCpu.ptr + static_cast<SIZE_T>(inc) * 2 };
            Resource nullResource;

            D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrvDesc = {};
            cubeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            cubeSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            cubeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            cubeSrvDesc.TextureCube.MipLevels = 1;
            cubeSrvDesc.TextureCube.MostDetailedMip = 0;
            cubeSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
            m_device->CreateShaderResourceView(nullResource, &cubeSrvDesc, iblCpu);
            m_device->CreateShaderResourceView(nullResource, &cubeSrvDesc, preCpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {};
            lutSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            lutSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            lutSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            lutSrvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(nullResource, &lutSrvDesc, brdfCpu);
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
                return failInit("Renderer::Initialize: Shadow map resource creation failed.\n");
            }

            D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
            dsvDesc.NumDescriptors = 1;
            dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeapShadow);
            if (FAILED(hr)) {
                return failInit("Renderer::Initialize: Shadow DSV heap creation failed.\n");
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

        if (!InitializeSkyboxGeometry()) {
            DebugLog("Skybox geometry initialization failed. Skybox pass will be disabled.\n");
        }

        if (!InitializeFrameContexts(GetBackBufferCount())) {
            return failInit("Renderer::Initialize: Frame context initialization failed.\n");
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
                return failInit("Renderer::Initialize: Main depth resource creation failed.\n");
            }

            D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
            dsvDesc.NumDescriptors = 1;
            dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeap);
            if (FAILED(hr)) {
                return failInit("Renderer::Initialize: Main DSV heap creation failed.\n");
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
            return failInit("Renderer::Initialize: Main command list creation failed.\n");
        }
        m_mainCommandList.Close();
        m_mainCommandListReady = true;

        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, m_frameFence.GetAddressOf());
        if (FAILED(hr)) {
            return failInit("Renderer::Initialize: Frame fence creation failed.\n");
        }

        m_frameFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_frameFenceEvent) {
            return failInit("Renderer::Initialize: Frame fence event creation failed.\n");
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
                DebugLog("Renderer::InitializeFrameContexts: CreateCommandAllocator failed.\n");
                return false;
            }

            if (!CreateUploadBuffer(*m_device,
                                    cameraCbSize,
                                    frame.cameraCB,
                                    reinterpret_cast<void**>(&frame.cameraCBPtr))) {
                DebugLog("Renderer::InitializeFrameContexts: Camera CB creation failed.\n");
                return false;
            }
            frame.cameraCbCapacity = 1;
            frame.cameraCbCount = 0;
            if (!CreateUploadBuffer(*m_device, lightCbSize, frame.lightCB, &frame.lightCBPtr)) {
                DebugLog("Renderer::InitializeFrameContexts: Light CB creation failed.\n");
                return false;
            }

            CpuDescriptorHandle lightSrvCpu{};
            GpuDescriptorHandle lightSrvGpu{};
            if (!AllocateSrvRange(2, lightSrvCpu, lightSrvGpu)) {
                DebugLog("Renderer::InitializeFrameContexts: SRV allocation failed for light buffers.\n");
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
            DebugLog("Failed to reset command allocator\n");
            return false;
        }

        hr = m_mainCommandList.Reset(m_frames[frameIndex].cmdAllocator, &m_rlConfig.GetPipelineState());
        if (FAILED(hr)) {
            DebugLog("Failed to reset command list\n");
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
            DebugLog("SRV heap exhausted\n");
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
        EnsureCameraBuffers(frame, static_cast<UINT>(m_drawItems.size() * 2u + 4u));

        EnsureIconTextureUploaded(cmdList);
        if (!m_skyboxTextureUploaded && !m_skyboxUploadAttempted) {
            EnsureSkyboxTextureUploaded(cmdList);
        } else if (!m_iblUploaded && !m_iblUploadAttempted) {
            // Avoid doing skybox and IBL generation in the same frame.
            EnsureIblTexturesUploaded(cmdList);
        }
        ShadowPass(cmdList, frame);
        TransitionBackBufferToRenderTarget(cmdList, backIndex);
        ClearAndBindMainTargets(cmdList, backIndex);
        MainPass(cmdList, frame);
        if (m_gBufferDebugView == GBufferDebugView::FinalLit) {
            SkyboxPass(cmdList, frame);
        }

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

        const bool useTessPath = (m_rasterShaderMode == RasterShaderMode::PBR) && m_useTessellation;
        cmdList->SetGraphicsRootSignature(m_rlConfig.GetRootSignature());
        if (useTessPath) {
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
        cmdList->IASetPrimitiveTopology(useTessPath ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                    : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto dsv = m_dsvHeapShadow->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        float lightVP[16];
        float fwd[3] = {};
        Math::BuildDirectionalLightViewProjection(m_lightYaw,
                                                  m_lightPitch,
                                                  m_lightDistance,
                                                  m_lightOrthoHalf,
                                                  m_lightNear,
                                                  m_lightFar,
                                                  lightVP,
                                                  fwd);

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
                    float spotDir[3] = {};
                    // Convert yaw/pitch angles to normalized spotlight forward direction.
                    Math::DirectionFromYawPitch(sl.yaw, sl.pitch, spotDir);
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
            cb.cameraPos[0] = m_cameraPos[0];
            cb.cameraPos[1] = m_cameraPos[1];
            cb.cameraPos[2] = m_cameraPos[2];
            cb.cameraPos[3] = 1.0f;
            cb.iblParams[0] = m_iblEnabled ? 1.0f : 0.0f;
            cb.iblParams[1] = m_iblIntensity;
            cb.iblParams[2] = m_iblPrefilterMaxMip;
            cb.iblParams[3] = 0.0f;
            cb.debugParams[0] = static_cast<float>(static_cast<int>(m_gBufferDebugView));
            cb.debugParams[1] = 0.0f;
            cb.debugParams[2] = 0.0f;
            cb.debugParams[3] = 0.0f;
            memcpy(frame.lightCBPtr, &cb, sizeof(cb));
        }

        cmdList->SetGraphicsRootConstantBufferView(3, frame.lightCB->GetGPUVirtualAddress());

        for (const auto& item : m_drawItems) {
            float objLightMVP[16];
            // Object -> light clip matrix:
            // objLightMVP = model * lightVP
            Mul4x4(item.model, lightVP, objLightMVP);
            const D3D12_GPU_VIRTUAL_ADDRESS cameraCbGpu = PushCameraCB(frame, objLightMVP, item.model);
            if (cameraCbGpu != 0) {
                cmdList->SetGraphicsRootConstantBufferView(2, cameraCbGpu);
            }
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
            DebugLog("ResizeBuffers failed\n");
            return;
        }

        if (!m_rtBinder.Initialize(*m_device, m_device->GetSwapChain(), GetBackBufferCount())) {
            DebugLog("RTV rebinding failed\n");
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
            DebugLog("Depth recreation failed\n");
            return;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr2 = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeap);
        if (FAILED(hr2)) {
            DebugLog("DSV heap recreation failed\n");
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        m_device->CreateDepthStencilView(m_depth, &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    void Renderer::RefreshEnvironmentAssets()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }

        m_skyboxTexture.Reset();
        m_skyboxTextureUpload.Reset();
        m_skyboxTextureUploaded = false;
        m_skyboxUploadAttempted = false;
        m_skyboxTextureIsHdr = false;

        m_iblIrradianceTexture.Reset();
        m_iblIrradianceUpload.Reset();
        m_iblPrefilterTexture.Reset();
        m_iblPrefilterUpload.Reset();
        m_iblBrdfLutTexture.Reset();
        m_iblBrdfLutUpload.Reset();
        m_iblUploaded = false;
        m_iblUploadAttempted = false;
        m_iblEnabled = false;
        m_iblPrefilterMaxMip = 0.0f;

        m_hdrEquirectLoaded = false;
        m_hdrEquirectTried = false;
        m_hdrEquirectWidth = 0;
        m_hdrEquirectHeight = 0;
        m_hdrEquirectPixels.clear();
    }

    void Renderer::SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height)
    {
        if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3u ||
            width == 0 || height == 0) {
            m_skyboxSourceType = SkyboxSourceType::None;
            m_skyboxSourceWidth = 0;
            m_skyboxSourceHeight = 0;
            m_skyboxSourceHdrRgb.clear();
            m_skyboxSourceLdrRgba8.clear();
            DebugLog("Renderer::SetSkyboxHdrEquirectData failed: invalid HDR source size.\n");
            return;
        }

        m_skyboxSourceType = SkyboxSourceType::HdrRgbFloat;
        m_skyboxSourceWidth = width;
        m_skyboxSourceHeight = height;
        m_skyboxSourceHdrRgb = std::move(pixels);
        m_skyboxSourceLdrRgba8.clear();
    }

    void Renderer::SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height)
    {
        if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u ||
            width == 0 || height == 0) {
            m_skyboxSourceType = SkyboxSourceType::None;
            m_skyboxSourceWidth = 0;
            m_skyboxSourceHeight = 0;
            m_skyboxSourceHdrRgb.clear();
            m_skyboxSourceLdrRgba8.clear();
            DebugLog("Renderer::SetSkyboxLdrEquirectData failed: invalid LDR source size.\n");
            return;
        }

        m_skyboxSourceType = SkyboxSourceType::LdrRgba8;
        m_skyboxSourceWidth = width;
        m_skyboxSourceHeight = height;
        m_skyboxSourceLdrRgba8 = std::move(pixels);
        m_skyboxSourceHdrRgb.clear();
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
        static const uint8_t kWhiteRgba[4] = { 255, 255, 255, 255 };
        if (!CreateTexture2DFromRgba8(*m_device, cmdList, kWhiteRgba, 1, 1, m_texture, m_textureUpload)) {
            DebugLog("Renderer::EnsureIconTextureUploaded: failed to create default 1x1 white texture.\n");
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_texture, &srvDesc, m_iconSrvCpu);

        m_iconTex.resource = m_texture;
        m_iconTex.desc.width = 1;
        m_iconTex.desc.height = 1;
        m_material.Set(TextureSlot::Albedo, &m_iconTex);
        m_textureUploaded = true;
    }

    bool Renderer::InitializeSkyboxGeometry()
    {
        const UINT64 vbBytes = sizeof(kSkyboxCubeVertices);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC vbDesc = {};
        vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vbDesc.Width = vbBytes;
        vbDesc.Height = 1;
        vbDesc.DepthOrArraySize = 1;
        vbDesc.MipLevels = 1;
        vbDesc.SampleDesc.Count = 1;
        vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = m_device->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &vbDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       m_skyboxVB);
        if (FAILED(hr)) {
            return false;
        }

        void* mapped = nullptr;
        hr = m_skyboxVB->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) {
            m_skyboxVB.Reset();
            return false;
        }
        memcpy(mapped, kSkyboxCubeVertices, sizeof(kSkyboxCubeVertices));
        m_skyboxVB->Unmap(0, nullptr);

        m_skyboxVBV.BufferLocation = m_skyboxVB->GetGPUVirtualAddress();
        m_skyboxVBV.StrideInBytes = sizeof(SkyboxVertex);
        m_skyboxVBV.SizeInBytes = static_cast<UINT>(sizeof(kSkyboxCubeVertices));
        return true;
    }

    bool Renderer::EnsureHdrEnvironmentLoaded()
    {
        if (m_hdrEquirectLoaded) {
            return true;
        }
        if (m_hdrEquirectTried) {
            return false;
        }
        m_hdrEquirectTried = true;

        if (m_skyboxLoadFormat == SkyboxLoadFormat::CubemapFaces) {
            DebugLog("EnsureHdrEnvironmentLoaded: CubemapFaces format selected, skipping equirect load.\n");
            return false;
        }

        if (m_skyboxSourceType == SkyboxSourceType::None ||
            m_skyboxSourceWidth == 0 || m_skyboxSourceHeight == 0) {
            DebugLog("EnsureHdrEnvironmentLoaded failed: skybox equirect input data is not set.\n");
            return false;
        }

        auto loadFromFloatRgb = [this](std::vector<float> pixels, UINT width, UINT height) -> bool {
            if (pixels.empty() || width == 0 || height == 0) {
                return false;
            }
            m_hdrEquirectWidth = width;
            m_hdrEquirectHeight = height;
            m_hdrEquirectPixels = std::move(pixels);
            m_hdrEquirectLoaded = true;
            return true;
        };

        auto tryLoadHdrFromInput = [&]() -> bool {
            if (m_skyboxSourceType != SkyboxSourceType::HdrRgbFloat) {
                return false;
            }
            if (m_skyboxSourceHdrRgb.size() !=
                static_cast<size_t>(m_skyboxSourceWidth) * static_cast<size_t>(m_skyboxSourceHeight) * 3u) {
                return false;
            }
            if (!loadFromFloatRgb(m_skyboxSourceHdrRgb, m_skyboxSourceWidth, m_skyboxSourceHeight)) {
                return false;
            }
            DebugLog("Loaded HDR equirect environment map for runtime skybox/IBL generation.\n");
            return true;
        };

        auto tryLoadLdrFromInput = [&]() -> bool {
            if (m_skyboxSourceType != SkyboxSourceType::LdrRgba8) {
                return false;
            }
            if (m_skyboxSourceLdrRgba8.size() !=
                static_cast<size_t>(m_skyboxSourceWidth) * static_cast<size_t>(m_skyboxSourceHeight) * 4u) {
                return false;
            }

            std::vector<float> pixelsRgb;
            pixelsRgb.resize(static_cast<size_t>(m_skyboxSourceWidth) * static_cast<size_t>(m_skyboxSourceHeight) * 3u);
            auto srgbToLinear = [](float x) -> float {
                // IEC 61966-2-1 sRGB -> linear transfer:
                // x_linear = x/12.92                      (x <= 0.04045)
                // x_linear = ((x + 0.055)/1.055)^2.4     (otherwise)
                if (x <= 0.04045f) {
                    return x / 12.92f;
                }
                return std::pow((x + 0.055f) / 1.055f, 2.4f);
            };
            for (size_t i = 0; i < static_cast<size_t>(m_skyboxSourceWidth) * static_cast<size_t>(m_skyboxSourceHeight); ++i) {
                const size_t src = i * 4u;
                const size_t dst = i * 3u;
                const float r = static_cast<float>(m_skyboxSourceLdrRgba8[src + 0]) / 255.0f;
                const float g = static_cast<float>(m_skyboxSourceLdrRgba8[src + 1]) / 255.0f;
                const float b = static_cast<float>(m_skyboxSourceLdrRgba8[src + 2]) / 255.0f;
                pixelsRgb[dst + 0] = srgbToLinear(r);
                pixelsRgb[dst + 1] = srgbToLinear(g);
                pixelsRgb[dst + 2] = srgbToLinear(b);
            }

            if (!loadFromFloatRgb(std::move(pixelsRgb), m_skyboxSourceWidth, m_skyboxSourceHeight)) {
                return false;
            }
            DebugLog("Loaded LDR equirect environment map for runtime skybox/IBL generation.\n");
            return true;
        };

        switch (m_skyboxLoadFormat) {
        case SkyboxLoadFormat::HdrEquirect:
            if (!tryLoadHdrFromInput()) {
                DebugLog("EnsureHdrEnvironmentLoaded failed: HdrEquirect mode requires HDR equirect input.\n");
                return false;
            }
            return true;
        case SkyboxLoadFormat::LdrEquirect:
            if (!tryLoadLdrFromInput()) {
                DebugLog("EnsureHdrEnvironmentLoaded failed: LdrEquirect mode requires LDR equirect input.\n");
                return false;
            }
            return true;
        case SkyboxLoadFormat::Auto:
        default:
            if (tryLoadHdrFromInput()) {
                return true;
            }
            if (tryLoadLdrFromInput()) {
                return true;
            }
            DebugLog("EnsureHdrEnvironmentLoaded failed: Auto mode requires HDR or LDR equirect input.\n");
            return false;
        }

        return false;
    }

    void Renderer::EnsureSkyboxTextureUploaded(CommandList* cmdList)
    {
        if (m_skyboxTextureUploaded || m_skyboxUploadAttempted) {
            return;
        }
        m_skyboxUploadAttempted = true;

        if (EnsureHdrEnvironmentLoaded()) {
            std::vector<std::vector<float>> skyFaces;
            const UINT skyFaceSize = 256;
            // Convert loaded equirectangular environment L(theta, phi)
            // into cubemap texels L(direction) for runtime skybox rendering.
            RendererMathUtility::GenerateSkyCubemapFromEquirect(m_hdrEquirectPixels,
                                                                m_hdrEquirectWidth,
                                                                m_hdrEquirectHeight,
                                                                skyFaceSize,
                                                                skyFaces);
            if (CreateTextureCubeFromFloatFacesWithMips(*m_device, cmdList, skyFaces, skyFaceSize, 1,
                                                        m_skyboxTexture, m_skyboxTextureUpload)) {
                D3D12_SHADER_RESOURCE_VIEW_DESC hdrSrvDesc = {};
                hdrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                hdrSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                hdrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                hdrSrvDesc.TextureCube.MipLevels = 1;
                hdrSrvDesc.TextureCube.MostDetailedMip = 0;
                hdrSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
                // Keep HDR skybox in FP16 (R16G16B16A16_FLOAT) to preserve dynamic range.
                m_device->CreateShaderResourceView(m_skyboxTexture, &hdrSrvDesc, m_skyboxSrvCpu);
                m_skyboxTextureUploaded = true;
                m_skyboxTextureIsHdr = true;
                return;
            }
            DebugLog("Equirect skybox generation failed. Falling back to cubemap faces.\n");
        }

        std::vector<std::vector<uint8_t>> facePixels;
        static const uint8_t fallbackFaces[6][4] = {
            { 200, 40, 40, 255 },   // +X
            { 40, 200, 40, 255 },   // -X
            { 40, 40, 200, 255 },   // +Y
            { 200, 200, 40, 255 },  // -Y
            { 40, 200, 200, 255 },  // +Z
            { 200, 40, 200, 255 },  // -Z
        };
        const UINT texWidth = 1;
        const UINT texHeight = 1;
        facePixels.resize(6);
        for (int i = 0; i < 6; ++i) {
            facePixels[i].assign(fallbackFaces[i], fallbackFaces[i] + 4);
        }
        DebugLog("Skybox uses built-in fallback cubemap colors.\n");
        if (!CreateTextureCubeFromRgba8Faces(*m_device, cmdList, facePixels, texWidth, texHeight,
                                             m_skyboxTexture, m_skyboxTextureUpload)) {
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = 1;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        m_device->CreateShaderResourceView(m_skyboxTexture, &srvDesc, m_skyboxSrvCpu);

        m_skyboxTextureUploaded = true;
        m_skyboxTextureIsHdr = false;
    }

    void Renderer::EnsureIblTexturesUploaded(CommandList* cmdList)
    {
        if (m_iblUploaded || m_iblUploadAttempted) {
            return;
        }
        m_iblUploadAttempted = true;

        auto resetIblResources = [&]() {
            m_iblIrradianceTexture.Reset();
            m_iblIrradianceUpload.Reset();
            m_iblPrefilterTexture.Reset();
            m_iblPrefilterUpload.Reset();
            m_iblBrdfLutTexture.Reset();
            m_iblBrdfLutUpload.Reset();
        };

        if (EnsureHdrEnvironmentLoaded()) {
            std::vector<std::vector<float>> irradianceFaces;
            std::vector<std::vector<float>> prefilterSubresources;
            std::vector<float> brdfLutPixels;

            const UINT irradianceSize = 16;
            const UINT prefilterSize = 64;
            const UINT prefilterMipLevels = 4;
            const UINT brdfLutSize = 64;

            // Runtime split-sum IBL preprocessing:
            // 1) Irradiance cubemap   : diffuse integral over hemisphere.
            // 2) Prefilter cubemap    : roughness-dependent GGX specular integral.
            // 3) BRDF LUT (A,B terms) : view-angle/roughness integration term.
            RendererMathUtility::GenerateIrradianceCubemapFromEquirect(m_hdrEquirectPixels,
                                                                       m_hdrEquirectWidth,
                                                                       m_hdrEquirectHeight,
                                                                       irradianceSize,
                                                                       irradianceFaces);
            RendererMathUtility::GeneratePrefilterCubemapFromEquirect(m_hdrEquirectPixels,
                                                                      m_hdrEquirectWidth,
                                                                      m_hdrEquirectHeight,
                                                                      prefilterSize,
                                                                      prefilterMipLevels,
                                                                      prefilterSubresources);
            RendererMathUtility::GenerateBrdfLut(brdfLutSize, brdfLutSize, brdfLutPixels);

            const bool irrOk = CreateTextureCubeFromFloatFacesWithMips(*m_device, cmdList, irradianceFaces,
                                                                        irradianceSize, 1,
                                                                        m_iblIrradianceTexture, m_iblIrradianceUpload);
            const bool preOk = CreateTextureCubeFromFloatFacesWithMips(*m_device, cmdList, prefilterSubresources,
                                                                        prefilterSize, prefilterMipLevels,
                                                                        m_iblPrefilterTexture, m_iblPrefilterUpload);
            const bool brdfOk = CreateTexture2DFromFloatRgba(*m_device, cmdList, brdfLutPixels, brdfLutSize, brdfLutSize,
                                                              m_iblBrdfLutTexture, m_iblBrdfLutUpload);
            if (irrOk && preOk && brdfOk) {
                const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                CpuDescriptorHandle irrCpu = m_iblSrvCpu;
                CpuDescriptorHandle preCpu = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) };
                CpuDescriptorHandle brdfCpu = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) * 2 };

                D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrvDesc = {};
                cubeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                cubeSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                cubeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                cubeSrvDesc.TextureCube.MipLevels = 1;
                cubeSrvDesc.TextureCube.MostDetailedMip = 0;
                cubeSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
                // Irradiance is low-frequency diffuse term (single mip is enough).
                m_device->CreateShaderResourceView(m_iblIrradianceTexture, &cubeSrvDesc, irrCpu);

                D3D12_SHADER_RESOURCE_VIEW_DESC preCubeSrvDesc = cubeSrvDesc;
                // Prefilter uses mip chain as roughness lookup: lod = roughness * maxMip.
                preCubeSrvDesc.TextureCube.MipLevels = prefilterMipLevels;
                m_device->CreateShaderResourceView(m_iblPrefilterTexture, &preCubeSrvDesc, preCpu);

                D3D12_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {};
                lutSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                lutSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                lutSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                lutSrvDesc.Texture2D.MipLevels = 1;
                m_device->CreateShaderResourceView(m_iblBrdfLutTexture, &lutSrvDesc, brdfCpu);

                m_iblEnabled = true;
                m_iblPrefilterMaxMip = static_cast<float>(prefilterMipLevels - 1);
                m_iblUploaded = true;
                return;
            }

            resetIblResources();
            DebugLog("Runtime equirect->IBL generation failed. Falling back to built-in IBL fallback textures.\n");
        }

        std::vector<std::vector<uint8_t>> irradianceFaces;
        std::vector<std::vector<uint8_t>> prefilterFaces;
        const UINT irrW = 1;
        const UINT irrH = 1;
        const UINT preW = 1;
        const UINT preH = 1;
        const UINT brdfW = 1;
        const UINT brdfH = 1;
        std::vector<uint8_t> brdfPixels;
        static const uint8_t blackFace[4] = { 0, 0, 0, 255 };
        static const uint8_t midFace[4] = { 128, 128, 128, 255 };

        irradianceFaces.assign(6, std::vector<uint8_t>(blackFace, blackFace + 4));
        prefilterFaces.assign(6, std::vector<uint8_t>(blackFace, blackFace + 4));
        brdfPixels.assign(midFace, midFace + 4);
        m_iblEnabled = false;
        DebugLog("IBL uses built-in fallback textures (runtime environment source unavailable).\n");

        if (!CreateTextureCubeFromRgba8Faces(*m_device, cmdList, irradianceFaces, irrW, irrH,
                                             m_iblIrradianceTexture, m_iblIrradianceUpload)) {
            return;
        }
        if (!CreateTextureCubeFromRgba8Faces(*m_device, cmdList, prefilterFaces, preW, preH,
                                             m_iblPrefilterTexture, m_iblPrefilterUpload)) {
            return;
        }
        if (!CreateTexture2DFromRgba8(*m_device, cmdList, brdfPixels.data(), brdfW, brdfH,
                                      m_iblBrdfLutTexture, m_iblBrdfLutUpload)) {
            return;
        }

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CpuDescriptorHandle irrCpu = m_iblSrvCpu;
        CpuDescriptorHandle preCpu = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) };
        CpuDescriptorHandle brdfCpu = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) * 2 };

        D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrvDesc = {};
        cubeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cubeSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cubeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cubeSrvDesc.TextureCube.MipLevels = 1;
        cubeSrvDesc.TextureCube.MostDetailedMip = 0;
        cubeSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        m_device->CreateShaderResourceView(m_iblIrradianceTexture, &cubeSrvDesc, irrCpu);
        m_device->CreateShaderResourceView(m_iblPrefilterTexture, &cubeSrvDesc, preCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {};
        lutSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lutSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lutSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lutSrvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_iblBrdfLutTexture, &lutSrvDesc, brdfCpu);

        m_iblPrefilterMaxMip = 0.0f;
        m_iblUploaded = true;
    }

    Texture* Renderer::CreateTextureFromRgba8Data(const CpuTextureRgba8& src, CommandList* cmdList,
                                                  std::vector<Resource>& uploads)
    {
        if (src.pixels.empty() || src.width == 0 || src.height == 0 || !cmdList) {
            return nullptr;
        }

        Resource texture;
        Resource upload;
        if (!CreateTexture2DFromRgba8(*m_device, cmdList, src.pixels.data(),
                                      src.width, src.height,
                                      texture, upload)) {
            return nullptr;
        }

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
        texObj->desc.width = src.width;
        texObj->desc.height = src.height;
        texObj->desc.mips = 1;
        texObj->desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;

        uploads.push_back(upload);
        m_sceneTextures.push_back(std::move(texObj));
        return m_sceneTextures.back().get();
    }

    void Renderer::EnsureCameraBuffers(FrameContext& frame, UINT requiredCount)
    {
        if (!m_device) {
            return;
        }

        const UINT cbSize = (sizeof(CameraCBData) + 255u) & ~255u;
        const UINT needed = (requiredCount > 0) ? requiredCount : 1u;
        if (frame.cameraCB.IsValid() &&
            frame.cameraCBPtr &&
            frame.cameraCbCapacity >= needed) {
            frame.cameraCbCount = 0;
            return;
        }

        UINT newCapacity = (frame.cameraCbCapacity > 0) ? frame.cameraCbCapacity : 1u;
        while (newCapacity < needed) {
            newCapacity *= 2u;
        }

        if (frame.cameraCB.IsValid() && frame.cameraCBPtr) {
            frame.cameraCB->Unmap(0, nullptr);
            frame.cameraCBPtr = nullptr;
        }
        frame.cameraCB.Reset();
        frame.cameraCbCapacity = 0;
        frame.cameraCbCount = 0;

        if (!CreateUploadBuffer(*m_device,
                                static_cast<UINT64>(cbSize) * static_cast<UINT64>(newCapacity),
                                frame.cameraCB,
                                reinterpret_cast<void**>(&frame.cameraCBPtr))) {
            DebugLog("Renderer::EnsureCameraBuffers: Camera CB grow failed.\n");
            return;
        }
        frame.cameraCbCapacity = newCapacity;
    }

    D3D12_GPU_VIRTUAL_ADDRESS Renderer::PushCameraCB(FrameContext& frame, const float mvp[16], const float world[16])
    {
        if (!frame.cameraCB.IsValid() || !frame.cameraCBPtr || frame.cameraCbCapacity == 0) {
            return 0;
        }

        const UINT cbSize = (sizeof(CameraCBData) + 255u) & ~255u;
        UINT slot = frame.cameraCbCount;
        if (slot >= frame.cameraCbCapacity) {
            slot = frame.cameraCbCapacity - 1;
        } else {
            ++frame.cameraCbCount;
        }

        CameraCBData* dst = reinterpret_cast<CameraCBData*>(
            frame.cameraCBPtr + static_cast<size_t>(cbSize) * static_cast<size_t>(slot));
        memcpy(dst->mvp, mvp, sizeof(dst->mvp));
        memcpy(dst->world, world, sizeof(dst->world));

        return frame.cameraCB->GetGPUVirtualAddress() + static_cast<UINT64>(cbSize) * static_cast<UINT64>(slot);
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
        const bool usePbrPath = (m_rasterShaderMode == RasterShaderMode::PBR);
        const bool useTessPath = usePbrPath && m_useTessellation;

        cmdList->SetGraphicsRootSignature(m_rlConfig.GetRootSignature());
        cmdList->RSSetViewports(1, &m_viewport);
        cmdList->RSSetScissorRects(1, &m_scissorRect);
        if (useTessPath) {
            cmdList->SetPipelineState(m_rlConfig.GetTessellationPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        } else if (usePbrPath) {
            cmdList->SetPipelineState(m_rlConfig.GetPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        } else {
            cmdList->SetPipelineState(m_rlConfig.GetBasicPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

        DescriptorHeap* heaps[] = { &m_srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);

        cmdList->SetGraphicsRootDescriptorTable(1, m_shadowSrv);
        cmdList->SetGraphicsRootDescriptorTable(4, frame.lightSrvTable);
        cmdList->SetGraphicsRootDescriptorTable(5, m_iblSrv);

        cmdList->SetGraphicsRootConstantBufferView(3, frame.lightCB->GetGPUVirtualAddress());
        for (const auto& item : m_drawItems) {
            float objMVP[16];
            // Object -> camera clip matrix:
            // objMVP = model * cameraVP
            Mul4x4(item.model, m_cameraPV, objMVP);
            const D3D12_GPU_VIRTUAL_ADDRESS cameraCbGpu = PushCameraCB(frame, objMVP, item.model);
            if (cameraCbGpu != 0) {
                cmdList->SetGraphicsRootConstantBufferView(2, cameraCbGpu);
            }
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

    void Renderer::SkyboxPass(CommandList* cmdList, FrameContext& frame)
    {
        if (!m_skyboxTextureUploaded || !m_skyboxVB.IsValid() || !frame.cameraCB.IsValid()) {
            return;
        }

        cmdList->SetGraphicsRootSignature(m_rlConfig.GetRootSignature());
        // Skybox PS is selected by sky texture domain:
        // - HDR (float cubemap): tone-map/gamma path
        // - LDR (UNORM cubemap): direct sample path
        bool useHdrShader = m_skyboxTextureIsHdr;
        if (m_skyboxLoadFormat == SkyboxLoadFormat::LdrEquirect ||
            m_skyboxLoadFormat == SkyboxLoadFormat::CubemapFaces) {
            useHdrShader = false;
        }

        if (useHdrShader) {
            cmdList->SetPipelineState(m_rlConfig.GetSkyboxHdrPipelineState());
        } else {
            cmdList->SetPipelineState(m_rlConfig.GetSkyboxLdrPipelineState());
        }
        cmdList->RSSetViewports(1, &m_viewport);
        cmdList->RSSetScissorRects(1, &m_scissorRect);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        DescriptorHeap* heaps[] = { &m_srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(0, m_skyboxSrv);

        float skyboxWorld[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            m_cameraPos[0], m_cameraPos[1], m_cameraPos[2], 1,
        };
        float skyboxMVP[16];
        // Skybox clip transform (camera-centered world * cameraVP).
        Mul4x4(skyboxWorld, m_cameraPV, skyboxMVP);
        const D3D12_GPU_VIRTUAL_ADDRESS cameraCbGpu = PushCameraCB(frame, skyboxMVP, skyboxWorld);
        if (cameraCbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(2, cameraCbGpu);
        }

        cmdList->IASetVertexBuffers(0, 1, &m_skyboxVBV);
        cmdList->DrawInstanced(static_cast<UINT>(_countof(kSkyboxCubeVertices)), 1, 0, 0);
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
            DebugLog("Failed to close command list\n");
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
            m_cameraPos[0] = camera->cameraPosition[0];
            m_cameraPos[1] = camera->cameraPosition[1];
            m_cameraPos[2] = camera->cameraPosition[2];
        } else {
            for (int i = 0; i < 16; ++i) {
                m_cameraPV[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            }
            m_cameraPos[0] = 0.0f;
            m_cameraPos[1] = 0.0f;
            m_cameraPos[2] = 0.0f;
        }
    }

    Texture* Renderer::ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData)
    {
        if (!textureData) {
            return nullptr;
        }

        const uint64_t textureId = textureData->id;
        if (textureId == 0) {
            return nullptr;
        }

        auto cached = m_textureCache.find(textureId);
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
        Texture* texture = CreateTextureFromRgba8Data(*textureData, &uploadList, uploads);
        if (!texture) {
            return nullptr;
        }

        uploadList->Close();
        ID3D12CommandList* lists[] = { uploadList.Get() };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, lists);
        m_device->WaitForGPU();
        uploads.clear();

        m_textureCache[textureId] = texture;
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
            item.texture = ResolveSceneTexture(proxy.albedoTexture);
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
