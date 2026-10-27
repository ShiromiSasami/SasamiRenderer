// Dx12GraphicsDeviceInit.cpp
// Destructor, Initialize, Getters, WaitForGPU.
#include "Renderer/Backends/DirectX12/Dx12GraphicsDevice.h"
#include "Renderer/Backends/DirectX12/Dx12ConversionUtils.h"
#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include <windows.h>
#include <debugapi.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstring>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Diagnostics/AdapterDiagnostics.h"


namespace SasamiEngine::Render
{
#if RHI_DIRECTX12

    namespace
    {
        // Detects whether the display currently backing `adapter`'s primary output is running in
        // HDR10 (PQ / Rec.2020) mode. DXGI_OUTPUT_DESC1::ColorSpace mirrors the desktop colour
        // space Windows is driving that monitor with: DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        // when Settings > System > Display > "Use HDR" is on for that monitor, some SDR colour
        // space (typically G22_NONE_P709) otherwise. This is the same signal used by Microsoft's
        // own DirectX-Graphics-Samples HDR sample (D3D12HDR) to gate HDR10 output.
        // Checking the swap chain's own CheckColorSpaceSupport (done later, right before
        // SetColorSpace1) additionally confirms the *swap chain* can carry that colour space, so
        // both checks are kept rather than relying on this one alone.
        bool DetectHdr10DisplaySupport(IDXGIAdapter1* adapter)
        {
            if (!adapter) {
                return false;
            }
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(0, &output))) {
                return false;
            }
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6))) {
                return false;
            }
            DXGI_OUTPUT_DESC1 desc1{};
            if (FAILED(output6->GetDesc1(&desc1))) {
                return false;
            }
            return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
    }

    Dx12GraphicsDevice::~Dx12GraphicsDevice()
    {
        if (m_fenceEvent) {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
    }

    bool Dx12GraphicsDevice::Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount)
    {
        m_bufferCount = bufferCount;

        auto cleanup = [&]() {
            if (m_fenceEvent) {
                CloseHandle(m_fenceEvent);
                m_fenceEvent = nullptr;
            }
            m_fence.Reset();
            m_swapChain.m_swapChain.Reset();
            m_computeQueue.m_queue.Reset();
            m_commandQueue.m_queue.Reset();
            m_rayTracingDevice.Reset();
            m_device.Reset();
            m_supportsHardwareRayTracing = false;
            m_capabilities = {};
        };


#if defined(_DEBUG)
        {
            ComPtr<ID3D12Debug> debugController;
            const HRESULT debugInterfaceResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
            if (FAILED(debugInterfaceResult)) {
                // Silence here is worse than the missing validation. D3D12GetDebugInterface fails
                // when the "Graphics Tools" optional Windows feature is absent, and until this
                // line existed a _DEBUG build simply ran with no debug layer and no indication of
                // it -- which reads exactly like "no validation errors" when grepping the log.
                // Install it with: DISM /Online /Add-Capability /CapabilityName:Tools.Graphics.DirectX~~~~0.0.1.0
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "Dx12GraphicsDevice::Initialize: D3D12 debug layer UNAVAILABLE "
                              "(D3D12GetDebugInterface hr=0x%08lX). Validation errors will not be "
                              "reported. Install the Graphics Tools optional feature.\n",
                              static_cast<unsigned long>(debugInterfaceResult));
                DebugLog(buf);
            } else {
                debugController->EnableDebugLayer();
                DebugLog("Dx12GraphicsDevice::Initialize: D3D12 debug layer enabled.\n");

                // GPU-based validation instruments every shader at first use, which
                // stalls the first real frame for tens of seconds on non-trivial scenes.
                // The plain debug layer above stays always-on in _DEBUG; GBV is opt-in
                // via SASAMI_GPU_VALIDATION=1 for deep-debugging sessions.
                ComPtr<ID3D12Debug1> debugController1;
                if (SUCCEEDED(debugController.As(&debugController1))) {
                    char gbvEnv[8]{};
                    const DWORD gbvLen =
                        GetEnvironmentVariableA("SASAMI_GPU_VALIDATION", gbvEnv, sizeof(gbvEnv));
                    if (gbvLen > 0 && gbvEnv[0] == '1') {
                        debugController1->SetEnableGPUBasedValidation(TRUE);
                        DebugLog("Dx12GraphicsDevice::Initialize: D3D12 GPU-based validation enabled.\n");
                    } else {
                        DebugLog("Dx12GraphicsDevice::Initialize: GPU-based validation off "
                                 "(set SASAMI_GPU_VALIDATION=1 to enable).\n");
                    }
                }
            }
        }

#endif
        ComPtr<IDXGIFactory6> factory;
        HRESULT result = CreateDXGIFactory(IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateDXGIFactory failed.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selectedLevel = static_cast<D3D_FEATURE_LEVEL>(0);
        for (D3D_FEATURE_LEVEL level : featureLevels) {
            result = D3D12CreateDevice(nullptr, level, IID_PPV_ARGS(&m_device));
            if (SUCCEEDED(result)) {
                selectedLevel = level;
                break;
            }
        }
        if (!m_device) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: D3D12CreateDevice failed for FL 12_1/12_0/11_1/11_0.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }
        {
            char msg[128] = {};
            std::snprintf(msg, sizeof(msg),
                          "Dx12GraphicsDevice::Initialize: D3D12 device created (feature level: 0x%04X).\n",
                          static_cast<unsigned int>(selectedLevel));
            DebugLog(msg);
        }

        {
            ComPtr<IDXGIAdapter1> selectedAdapter;
            if (SUCCEEDED(factory->EnumAdapterByLuid(m_device->GetAdapterLuid(), IID_PPV_ARGS(&selectedAdapter)))) {
                LogAdapterDiagnostics(selectedAdapter.Get());
                m_supportsHdr10Output = DetectHdr10DisplaySupport(selectedAdapter.Get());
                DebugLog(m_supportsHdr10Output
                    ? "Dx12GraphicsDevice::Initialize: primary output reports HDR10 (PQ/Rec.2020) "
                      "colour space; render.hdr can enable HDR10 swap chain output.\n"
                    : "Dx12GraphicsDevice::Initialize: primary output does not report HDR10 "
                      "colour space; HDR10 swap chain output will stay disabled regardless of "
                      "RenderSettings::enableHdrOutput.\n");
            }
        }

        m_supportsHardwareRayTracing = false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                    &options5,
                                                    sizeof(options5))) &&
            options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 &&
            SUCCEEDED(m_device.As(&m_rayTracingDevice))) {
            m_supportsHardwareRayTracing = true;
            DebugLog("Dx12GraphicsDevice::Initialize: hardware ray tracing is supported.\n");
        } else {
            m_rayTracingDevice.Reset();
            DebugLog("Dx12GraphicsDevice::Initialize: hardware ray tracing is not supported.\n");
        }

        // Mesh Shader Tier 1, probed here so RhiBackendCapabilities carries it. The PSO cache
        // used to be the only place that asked, which left supportsMeshShaders false even on
        // hardware that supports them, and anything reading the capability got the wrong answer.
        m_supportsMeshShaders = false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                    &options7,
                                                    sizeof(options7))) &&
            options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
            m_supportsMeshShaders = true;
            DebugLog("Dx12GraphicsDevice::Initialize: mesh shaders are supported.\n");
        } else {
            DebugLog("Dx12GraphicsDevice::Initialize: mesh shaders are not supported.\n");
        }

        D3D12_COMMAND_QUEUE_DESC cqDesc = {};
        cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result = m_device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&m_commandQueue.m_queue));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateCommandQueue failed.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC cqComputeDesc = {};
        cqComputeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        result = m_device->CreateCommandQueue(&cqComputeDesc, IID_PPV_ARGS(&m_computeQueue.m_queue));
        if (FAILED(result)) {
            DebugLog("Dx12GraphicsDevice::Initialize: CreateCommandQueue (COMPUTE) failed. Async compute will be unavailable.\n");
            m_computeQueue.m_queue.Reset(); // non-fatal: fall back to no compute queue
        }

        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.BufferCount = m_bufferCount;
        scDesc.Width = width;
        scDesc.Height = height;
        // Always created SDR here, even when RenderSettings::enableHdrOutput is already true:
        // Initialize() only gets a raw HWND/size/bufferCount (IRHIDevice::Initialize is shared by
        // all four backends, so its signature cannot grow a settings parameter without touching
        // Vulkan/DirectX11/OpenGL too). HDR10 is switched on afterwards, at the next
        // Renderer::ResizeViewport call, which does have RenderSettings and recreates the swap
        // chain at DXGI_FORMAT_R10G10B10A2_UNORM + SetColorSpace1 when enableHdrOutput is on and
        // SupportsHdr10Output() agrees. This also guarantees the default (off) path is byte-for-
        // byte identical to before this feature existed: this line never changes.
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain1;
        factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

        result = factory->CreateSwapChainForHwnd(m_commandQueue.m_queue.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateSwapChainForHwnd failed.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            swapChain1.Reset();
            cleanup();
            return false;
        }
        swapChain1.As(&m_swapChain.m_swapChain);

        result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateFence failed.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }
        m_fenceValue = 0;
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateEvent failed.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }

        m_capabilities = {};
        m_capabilities.api = RhiBackendApi::DirectX12;
        m_capabilities.supportsGraphicsQueue = true;
        m_capabilities.supportsComputeQueue = (m_computeQueue.m_queue != nullptr);
        m_capabilities.supportsSwapChain = true;
        m_capabilities.supportsNativeFrame = false;
        m_capabilities.supportsFeatureRenderPasses = true;
        m_capabilities.supportsD3D12CompatibilitySurface = true;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;
        m_capabilities.supportsHardwareRayTracing    = m_supportsHardwareRayTracing;
        m_capabilities.supportsRayTracingPipeline    = m_supportsHardwareRayTracing;
        m_capabilities.supportsMeshShaders           = m_supportsMeshShaders;
        // D3D12 device methods and PSO creation are free-threaded (Microsoft D3D12 docs).
        m_capabilities.supportsThreadedResourceCreation = true;
        m_capabilities.supportsThreadedPipelineCreation = true;

        return true;
    }

    GraphicsRuntime Dx12GraphicsDevice::GetBackend() const
    {
        return GraphicsRuntime::DirectX12;
    }

    void* Dx12GraphicsDevice::GetNativeDeviceHandle() const
    {
        return m_device.Get();
    }

    void* Dx12GraphicsDevice::GetNativeGraphicsQueueHandle() const
    {
        return m_commandQueue.m_queue.Get();
    }

    ID3D12Device* Dx12GraphicsDevice::GetDevice() const
    {
        return m_device.Get();
    }

    ID3D12Device5* Dx12GraphicsDevice::GetRayTracingDevice() const
    {
        return m_rayTracingDevice.Get();
    }

    const RhiBackendCapabilities& Dx12GraphicsDevice::GetCapabilities() const
    {
        return m_capabilities;
    }

    bool Dx12GraphicsDevice::SupportsHardwareRayTracing() const
    {
        return m_supportsHardwareRayTracing;
    }

    CommandQueue& Dx12GraphicsDevice::GetCommandQueue()
    {
        return m_commandQueue;
    }

    CommandQueue& Dx12GraphicsDevice::GetComputeQueue()
    {
        return m_computeQueue;
    }

    SwapChain& Dx12GraphicsDevice::GetSwapChain()
    {
        return m_swapChain;
    }

    UINT Dx12GraphicsDevice::GetDescriptorHandleIncrementSize(DescriptorHeapType type) const
    {
        return m_device->GetDescriptorHandleIncrementSize(type);
    }

    void Dx12GraphicsDevice::WaitForGPU()
    {
        if (!m_commandQueue.m_queue || !m_fence) return;
        const UINT64 v = ++m_fenceValue;
        if (FAILED(m_commandQueue.m_queue->Signal(m_fence.Get(), v))) return;
        if (m_fence->GetCompletedValue() < v) {
            if (FAILED(m_fence->SetEventOnCompletion(v, m_fenceEvent))) return;
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    bool Dx12GraphicsDevice::SupportsHdr10Output() const
    {
        return m_supportsHdr10Output;
    }

    bool Dx12GraphicsDevice::SetHdr10ColorSpaceEnabled(bool enable)
    {
        if (!m_swapChain.Get()) {
            return false;
        }

        const DXGI_COLOR_SPACE_TYPE colorSpace = enable
            ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020  // HDR10: PQ (ST 2084) transfer, Rec.2020 primaries
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;    // SDR: gamma 2.2, Rec.709 primaries (this engine's prior default)

        if (enable) {
            // Required before SetColorSpace1: a colour space DXGI_OUTPUT_DESC1 reports as the
            // desktop's current mode is not automatically guaranteed to be one this specific swap
            // chain (its format, flip model, etc.) can present in.
            UINT colorSpaceSupport = 0;
            if (FAILED(m_swapChain->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport)) ||
                (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
                DebugLog("Dx12GraphicsDevice::SetHdr10ColorSpaceEnabled: swap chain does not report "
                         "DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 support; falling back to SDR.\n");
                m_swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                return false;
            }
        }

        if (FAILED(m_swapChain->SetColorSpace1(colorSpace))) {
            DebugLog("Dx12GraphicsDevice::SetHdr10ColorSpaceEnabled: SetColorSpace1 failed.\n");
            return false;
        }
        return enable;
    }

#endif
} // namespace SasamiEngine::Render
