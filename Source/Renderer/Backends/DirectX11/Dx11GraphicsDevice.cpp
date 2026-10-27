#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/DirectX11/Dx11ConversionUtils.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Capture/PngWriter.h"
#include "Renderer/Structures/Skeleton.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <utility>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiEngine::Render
{
    namespace
    {

        static_assert((sizeof(Dx11RayMarchConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");
        static_assert((sizeof(Dx11MeshConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");

        // Opt-in: SASAMI_DX11_AO_DEBUG=1 paints the generated ambient-occlusion term over the
        // finished frame instead of leaving it only as a factor inside the lighting. TODO.md's
        // reverted Vulkan SSAO attempt recorded the lesson this exists for -- a path that shows AO
        // on its own is what lets a wrong AO term be told apart from wrong lighting.
        // GetEnvironmentVariableA rather than std::getenv, matching SASAMI_GPU_VALIDATION.
        bool IsNativeAoDebugViewEnabled()
        {
            static const bool enabled = [] {
                char value[8] = {};
                const DWORD length = GetEnvironmentVariableA("SASAMI_DX11_AO_DEBUG", value, sizeof(value));
                return length > 0 && length < static_cast<DWORD>(sizeof(value)) && value[0] == '1';
            }();
            return enabled;
        }







        UINT ToDx11TextureBindFlags(RhiTextureUsageFlags usage)
        {
            UINT flags = 0;
            if (HasFlag(usage, RhiTextureUsageFlags::ShaderResource)) {
                flags |= D3D11_BIND_SHADER_RESOURCE;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::RenderTarget)) {
                flags |= D3D11_BIND_RENDER_TARGET;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::DepthStencil)) {
                flags |= D3D11_BIND_DEPTH_STENCIL;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::UnorderedAccess)) {
                flags |= D3D11_BIND_UNORDERED_ACCESS;
            }
            return flags;
        }

        UINT ToDx11BufferBindFlags(RhiBufferUsageFlags usage)
        {
            UINT flags = 0;
            if (HasFlag(usage, RhiBufferUsageFlags::Vertex)) {
                flags |= D3D11_BIND_VERTEX_BUFFER;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::Index)) {
                flags |= D3D11_BIND_INDEX_BUFFER;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::Constant)) {
                flags |= D3D11_BIND_CONSTANT_BUFFER;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::ShaderResource) ||
                HasFlag(usage, RhiBufferUsageFlags::Structured)) {
                flags |= D3D11_BIND_SHADER_RESOURCE;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::UnorderedAccess)) {
                flags |= D3D11_BIND_UNORDERED_ACCESS;
            }
            return flags;
        }

        D3D11_SRV_DIMENSION ToDx11SrvDimension(RhiTextureViewDimension dimension)
        {
            switch (dimension) {
            case RhiTextureViewDimension::Texture2DArray: return D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            case RhiTextureViewDimension::TextureCube: return D3D11_SRV_DIMENSION_TEXTURECUBE;
            case RhiTextureViewDimension::Texture2D:
            default: return D3D11_SRV_DIMENSION_TEXTURE2D;
            }
        }

        D3D11_COMPARISON_FUNC ToDx11CompareOp(RhiCompareOp op)
        {
            switch (op) {
            case RhiCompareOp::Never: return D3D11_COMPARISON_NEVER;
            case RhiCompareOp::Less: return D3D11_COMPARISON_LESS;
            case RhiCompareOp::Equal: return D3D11_COMPARISON_EQUAL;
            case RhiCompareOp::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
            case RhiCompareOp::Greater: return D3D11_COMPARISON_GREATER;
            case RhiCompareOp::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
            case RhiCompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
            case RhiCompareOp::Always:
            default: return D3D11_COMPARISON_ALWAYS;
            }
        }

        D3D11_CULL_MODE ToDx11CullMode(RhiCullMode mode)
        {
            switch (mode) {
            case RhiCullMode::None: return D3D11_CULL_NONE;
            case RhiCullMode::Front: return D3D11_CULL_FRONT;
            case RhiCullMode::Back:
            default: return D3D11_CULL_BACK;
            }
        }

        D3D11_FILL_MODE ToDx11FillMode(RhiFillMode mode)
        {
            return mode == RhiFillMode::Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        }



    }


    Dx11GraphicsDevice::~Dx11GraphicsDevice()
    {
        Cleanup();
    }

    bool Dx11GraphicsDevice::Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount)
    {
        Cleanup();

        DXGI_SWAP_CHAIN_DESC swapDesc{};
        swapDesc.BufferCount = bufferCount;
        swapDesc.BufferDesc.Width = width;
        swapDesc.BufferDesc.Height = height;
        swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.OutputWindow = hWnd;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.SampleDesc.Quality = 0;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createFlags = 0;
#if defined(_DEBUG)
        createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL createdLevel{};

        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                                   D3D_DRIVER_TYPE_HARDWARE,
                                                   nullptr,
                                                   createFlags,
                                                   requestedLevels,
                                                   _countof(requestedLevels),
                                                   D3D11_SDK_VERSION,
                                                   &swapDesc,
                                                   m_swapChain.GetAddressOf(),
                                                   m_device.GetAddressOf(),
                                                   &createdLevel,
                                                   m_context.GetAddressOf());
        if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG)) {
            createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                               D3D_DRIVER_TYPE_HARDWARE,
                                               nullptr,
                                               createFlags,
                                               requestedLevels,
                                               _countof(requestedLevels),
                                               D3D11_SDK_VERSION,
                                               &swapDesc,
                                               m_swapChain.GetAddressOf(),
                                               m_device.GetAddressOf(),
                                               &createdLevel,
                                               m_context.GetAddressOf());
        }
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::Initialize: D3D11CreateDeviceAndSwapChain failed.\n");
            Cleanup();
            return false;
        }

        if (!CreateBackBufferView()) {
            Cleanup();
            return false;
        }

        m_capabilities = {};
        m_capabilities.api = RhiBackendApi::DirectX11;
        m_capabilities.supportsGraphicsQueue = true;
        m_capabilities.supportsComputeQueue = createdLevel >= D3D_FEATURE_LEVEL_11_0;
        m_capabilities.supportsSwapChain = true;
        m_capabilities.supportsNativeFrame = true;
        m_capabilities.supportsFeatureRenderPasses = false;
        m_capabilities.supportsD3D12CompatibilitySurface = false;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;
        // ID3D11Device is thread-safe; the immediate context is not and is never touched off-thread.
        m_capabilities.supportsThreadedResourceCreation = true;
        m_capabilities.supportsThreadedPipelineCreation = true;
        return true;
    }

    GraphicsRuntime Dx11GraphicsDevice::GetBackend() const
    {
        return GraphicsRuntime::DirectX11;
    }

    void* Dx11GraphicsDevice::GetNativeDeviceHandle() const
    {
        return m_device.Get();
    }

    void* Dx11GraphicsDevice::GetNativeGraphicsQueueHandle() const
    {
        return m_context.Get();
    }

    bool Dx11GraphicsDevice::GetOverlayDeviceContext(RhiOverlayDeviceContext& context)
    {
        if (!m_device || !m_context) {
            return false;
        }

        context.api = RhiBackendApi::DirectX11;
        // ImGui's DX11 backend expects `device` = ID3D11Device* and `queue` = ID3D11DeviceContext*.
        context.device = m_device.Get();
        context.queue = m_context.Get();
        return true;
    }

    ID3D12Device* Dx11GraphicsDevice::GetDevice() const
    {
        return nullptr;
    }

    ID3D12Device5* Dx11GraphicsDevice::GetRayTracingDevice() const
    {
        return nullptr;
    }

    const RhiBackendCapabilities& Dx11GraphicsDevice::GetCapabilities() const
    {
        return m_capabilities;
    }

    bool Dx11GraphicsDevice::SupportsHardwareRayTracing() const
    {
        return false;
    }

    CommandQueue& Dx11GraphicsDevice::GetCommandQueue()
    {
        return m_emptyGraphicsQueue;
    }

    CommandQueue& Dx11GraphicsDevice::GetComputeQueue()
    {
        return m_emptyComputeQueue;
    }

    SwapChain& Dx11GraphicsDevice::GetSwapChain()
    {
        return m_emptySwapChain;
    }

    UINT Dx11GraphicsDevice::GetDescriptorHandleIncrementSize(DescriptorHeapType) const
    {
        return 0;
    }

    void Dx11GraphicsDevice::WaitForGPU()
    {
        if (m_context) {
            m_context->Flush();
        }
    }


    bool Dx11GraphicsDevice::ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc)
    {
        if (!m_context || !m_swapChain || !m_backBufferRtv || !frameDesc.present) {
            return false;
        }

        const float clear[] = {
            frameDesc.clearColor.r,
            frameDesc.clearColor.g,
            frameDesc.clearColor.b,
            frameDesc.clearColor.a,
        };
        ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->ClearRenderTargetView(m_backBufferRtv.Get(), clear);
        // Cleared up front rather than per branch: only the mesh path produces ambient occlusion,
        // and a stale `true` left over from an earlier frame would have the mesh shader sample an
        // AO texture that was never written this frame.
        m_ssaoReadyThisFrame = false;
        if (frameDesc.rayMarch.enabled) {
            (void)RenderRayMarchFrame(frameDesc.rayMarch);
        } else if (frameDesc.mesh.enabled) {
            // Casters go in first: the shadow pass binds its own depth-only target, so it has to
            // finish before either mesh pass takes the back buffer.
            m_shadowMapReadyThisFrame = RenderNativeShadowMap(frameDesc.mesh);
            // Sky first, so the meshes draw over it. It replaces the flat clear colour as the
            // background; drawing it after the meshes would paint over them.
            RenderNativeSky(frameDesc.mesh);
            // Ambient occlusion has to finish before the shaded passes start: it is an input to
            // their lighting, and like the shadow pass it binds targets of its own, so it cannot
            // run while the back buffer is bound.
            m_ssaoReadyThisFrame = RenderNativeAmbientOcclusion(frameDesc.mesh);
            (void)RenderMeshFrame(frameDesc.mesh);
            (void)RenderSkinnedMeshFrame(frameDesc.mesh);
            if (m_ssaoReadyThisFrame && IsNativeAoDebugViewEnabled()) {
                m_ssaoGenerator.DrawDebugView(m_context.Get(),
                                              m_backBufferRtv.Get(),
                                              m_backBufferDepthWidth,
                                              m_backBufferDepthHeight);
            }
        }
        if (frameDesc.overlay) {
            // RenderMeshFrame / RenderSkinnedMeshFrame leave m_backBufferDsv bound; rebind the
            // back buffer without a depth view so the UI overlay never gets depth-tested away.
            m_context->OMSetRenderTargets(1, &rtv, nullptr);
            RhiOverlayFrameContext overlayContext{};
            overlayContext.api = RhiBackendApi::DirectX11;
            overlayContext.width = m_backBufferDepthWidth;
            overlayContext.height = m_backBufferDepthHeight;
            frameDesc.overlay->RecordOverlay(overlayContext);
        }
        if (frameDesc.capture && frameDesc.capture->path) {
            // Must run before Present: the swap chain uses DXGI_SWAP_EFFECT_DISCARD, so the back
            // buffer content is undefined right after Present and there is nothing left to read.
            frameDesc.capture->succeeded = CaptureBackBufferToPng(frameDesc.capture->path);
            frameDesc.capture->completed = true;
        }
        // Present has success codes that mean "nothing reached the screen" -- DXGI_STATUS_OCCLUDED
        // above all, which SUCCEEDED() happily accepts. Without this the app runs at full speed,
        // logs healthy frame times and shows a blank window, with nothing anywhere saying why.
        const HRESULT presentHr = m_swapChain->Present(1, 0);
        if (presentHr != S_OK) {
            if (presentHr != m_lastPresentStatus) {
                char message[128];
                snprintf(message,
                         sizeof(message),
                         "Dx11GraphicsDevice::ExecuteBackendFrame: Present returned 0x%08lX.\n",
                         static_cast<unsigned long>(presentHr));
                DebugLog(message);
            }
        }
        m_lastPresentStatus = presentHr;
        return SUCCEEDED(presentHr);
    }

    bool Dx11GraphicsDevice::RenderBackendClearFrame(const float clearColor[4])
    {
        RhiBackendFrameDesc frameDesc{};
        if (clearColor) {
            frameDesc.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
        }
        frameDesc.present = true;
        return ExecuteBackendFrame(frameDesc);
    }

    bool Dx11GraphicsDevice::ResizeBackendSwapChain(UINT width, UINT height)
    {
        if (!m_swapChain || !m_device || width == 0 || height == 0) {
            return false;
        }

        if (m_context) {
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            m_context->Flush();
        }

        m_backBufferDsv.Reset();
        m_backBufferDepth.Reset();
        m_backBufferDepthWidth = 0;
        m_backBufferDepthHeight = 0;
        m_backBufferRtv.Reset();
        HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            (void)CreateBackBufferView();
            DebugLog("Dx11GraphicsDevice::ResizeBackendSwapChain: ResizeBuffers failed.\n");
            return false;
        }

        return CreateBackBufferView();
    }


    HRESULT Dx11GraphicsDevice::CreateDescriptorHeap(const DescriptorHeapDesc&, DescriptorHeap&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateCommittedResource(const HeapProperties*,
                                                        HeapFlags,
                                                        const ResourceDesc*,
                                                        ResourceState,
                                                        const ClearValue*,
                                                        Resource&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateCommandAllocator(CommandListType, CommandAllocator&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateCommandList(UINT, CommandListType, CommandAllocator&, PipelineState*, CommandList&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateGraphicsPipelineState(const GraphicsPipelineDesc&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreatePipelineStateFromStream(const void*, size_t, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateRootSignature(UINT, const void*, size_t, RootSignature&)
    {
        return E_NOTIMPL;
    }

    void Dx11GraphicsDevice::CreateShaderResourceView(Resource&, const ShaderResourceViewDesc*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateDepthStencilView(Resource&, const DepthStencilViewDesc*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateRenderTargetView(Resource&, const D3D12_RENDER_TARGET_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateSampler(const D3D12_SAMPLER_DESC*, CpuDescriptorHandle)
    {
    }

    HRESULT Dx11GraphicsDevice::CreateFence(UINT64, D3D12_FENCE_FLAGS, ID3D12Fence**)
    {
        return E_NOTIMPL;
    }

    bool Dx11GraphicsDevice::CreateBackBufferView()
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::CreateBackBufferView: GetBuffer failed.\n");
            return false;
        }

        hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_backBufferRtv.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::CreateBackBufferView: CreateRenderTargetView failed.\n");
            return false;
        }
        return true;
    }

    bool Dx11GraphicsDevice::CaptureBackBufferToPng(const wchar_t* path)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::CaptureBackBufferToPng: GetBuffer failed.\n");
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.SampleDesc.Count = 1;

        // Staging textures are cheap relative to how rarely a screenshot is taken, so there is no
        // point keeping one alive across frames and having to manage its lifetime/resize.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        hr = m_device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::CaptureBackBufferToPng: CreateTexture2D (staging) failed.\n");
            return false;
        }

        m_context->CopyResource(staging.Get(), backBuffer.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::CaptureBackBufferToPng: Map failed.\n");
            return false;
        }

        // SavePngBgra8 hands its buffer to WIC as 32bppBGRA, so a back buffer that is already in
        // BGRA order passes straight through and an RGBA one is the case that needs swapping.
        // Getting this backwards is invisible in greys and only shows up as blue/orange UI accents.
        bool swapRedBlue = false;
        if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            swapRedBlue = true;
        } else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            swapRedBlue = false;
        } else {
            DebugLog("Dx11GraphicsDevice::CaptureBackBufferToPng: unsupported back buffer format.\n");
            m_context->Unmap(staging.Get(), 0);
            return false;
        }

        const uint32_t width = desc.Width;
        const uint32_t height = desc.Height;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        const uint8_t* srcBase = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t* dstRow = pixels.data() + static_cast<size_t>(y) * width * 4;
            if (swapRedBlue) {
                for (uint32_t x = 0; x < width; ++x) {
                    dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                    dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                    dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
                }
            } else {
                std::memcpy(dstRow, srcRow, static_cast<size_t>(width) * 4);
            }
        }

        m_context->Unmap(staging.Get(), 0);

        return SavePngBgra8(path, pixels.data(), width, height);
    }

    void Dx11GraphicsDevice::Cleanup()
    {
        if (m_context) {
            m_context->ClearState();
            m_context->Flush();
        }
        m_backBufferRtv.Reset();
        m_backBufferDsv.Reset();
        m_backBufferDepth.Reset();
        m_backBufferDepthWidth = 0;
        m_backBufferDepthHeight = 0;
        m_rayMarchVertexShader.Reset();
        m_rayMarchPixelShader.Reset();
        m_rayMarchConstantBuffer.Reset();
        m_meshVertexShader.Reset();
        m_meshPixelShader.Reset();
        m_meshInputLayout.Reset();
        m_meshConstantBuffer.Reset();
        m_meshSamplerState.Reset();
        m_meshRasterizerState.Reset();
        m_meshDepthStencilState.Reset();
        m_meshBlendState.Reset();
        m_meshOpaqueBlendState.Reset();
        m_meshTransparentDepthState.Reset();
        m_skinnedMeshVertexShader.Reset();
        m_skinnedMeshInputLayout.Reset();
        m_skinnedMeshConstantBuffer.Reset();
        m_skinnedMeshBoneConstantBuffer.Reset();
        m_swapChain.Reset();
        m_rhiResources.clear();
        m_rhiShaders.clear();
        m_rhiPipelineLayouts.clear();
        m_rhiPipelines.clear();
        m_rhiSrvs.clear();
        m_rhiRtvs.clear();
        m_rhiDsvs.clear();
        m_rhiViewResources.clear();
        m_nextRhiResourceHandle = 1;
        m_nextRhiDescriptorHandle = 1;
        m_nextRhiShaderHandle = 1;
        m_nextRhiPipelineLayoutHandle = 1;
        m_nextRhiPipelineHandle = 1;
        m_context.Reset();
        m_device.Reset();
        m_capabilities = {};
    }
}

#endif
