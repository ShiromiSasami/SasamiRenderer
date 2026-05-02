#include "Renderer/Core/Dx12GraphicsDevice.h"
#include <windows.h>
#include <debugapi.h>
#include <cstdio>

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
#if RHI_DIRECTX12
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
            m_commandQueue.m_queue.Reset();
            m_rayTracingDevice.Reset();
            m_device.Reset();
            m_supportsHardwareRayTracing = false;
        };

        ComPtr<IDXGIFactory6> factory;
        HRESULT result = CreateDXGIFactory(IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateDXGIFactory failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
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
            DebugLogDialog("Dx12GraphicsDevice::Initialize: D3D12CreateDevice failed for FL 12_1/12_0/11_1/11_0.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
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

        D3D12_COMMAND_QUEUE_DESC cqDesc = {};
        cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result = m_device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&m_commandQueue.m_queue));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateCommandQueue failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
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
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain1;
        factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

        result = factory->CreateSwapChainForHwnd(m_commandQueue.m_queue.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateSwapChainForHwnd failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            swapChain1.Reset();
            cleanup();
            return false;
        }
        swapChain1.As(&m_swapChain.m_swapChain);

        result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateFence failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }
        m_fenceValue = 0;
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateEvent failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }

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
            WaitForSingleObject(m_fenceEvent, 5000);
        }
    }

    HRESULT Dx12GraphicsDevice::CreateDescriptorHeap(const DescriptorHeapDesc& desc, DescriptorHeap& out)
    {
        return m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out.m_heap));
    }

    HRESULT Dx12GraphicsDevice::CreateCommittedResource(const HeapProperties* heapProps,
                                                        HeapFlags heapFlags,
                                                        const ResourceDesc* desc,
                                                        ResourceState initialState,
                                                        const ClearValue* clearValue,
                                                        Resource& out)
    {
        return m_device->CreateCommittedResource(heapProps, heapFlags, desc, initialState, clearValue, IID_PPV_ARGS(&out.m_resource));
    }

    HRESULT Dx12GraphicsDevice::CreateCommandAllocator(CommandListType type, CommandAllocator& out)
    {
        return m_device->CreateCommandAllocator(type, IID_PPV_ARGS(&out.m_allocator));
    }

    HRESULT Dx12GraphicsDevice::CreateCommandList(UINT nodeMask,
                                                  CommandListType type,
                                                  CommandAllocator& allocator,
                                                  PipelineState* initialPSO,
                                                  CommandList& out)
    {
        return m_device->CreateCommandList(nodeMask,
                                           type,
                                           allocator.Get(),
                                           initialPSO ? initialPSO->Get() : nullptr,
                                           IID_PPV_ARGS(&out.m_list));
    }

    HRESULT Dx12GraphicsDevice::CreateGraphicsPipelineState(const GraphicsPipelineDesc& desc, PipelineState& out)
    {
        return m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out.m_state));
    }

    HRESULT Dx12GraphicsDevice::CreatePipelineStateFromStream(const void* streamData, size_t streamSize, PipelineState& out)
    {
        ComPtr<ID3D12Device2> device2;
        HRESULT hr = m_device.As(&device2);
        if (FAILED(hr)) {
            return hr;
        }
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
        streamDesc.SizeInBytes                   = streamSize;
        streamDesc.pPipelineStateSubobjectStream = const_cast<void*>(streamData);
        return device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&out.m_state));
    }

    HRESULT Dx12GraphicsDevice::CreateRootSignature(UINT nodeMask, const void* blobData, size_t blobSize, RootSignature& out)
    {
        return m_device->CreateRootSignature(nodeMask, blobData, blobSize, IID_PPV_ARGS(&out.m_signature));
    }

    void Dx12GraphicsDevice::CreateShaderResourceView(Resource& resource, const ShaderResourceViewDesc* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateShaderResourceView(resource.Get(), desc, dest);
    }

    void Dx12GraphicsDevice::CreateDepthStencilView(Resource& resource, const DepthStencilViewDesc* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateDepthStencilView(resource.Get(), desc, dest);
    }

    void Dx12GraphicsDevice::CreateRenderTargetView(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateRenderTargetView(resource.Get(), desc, dest);
    }

    void Dx12GraphicsDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateConstantBufferView(desc, dest);
    }

    void Dx12GraphicsDevice::CreateSampler(const D3D12_SAMPLER_DESC* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateSampler(desc, dest);
    }

    HRESULT Dx12GraphicsDevice::CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags, ID3D12Fence** fence)
    {
        return m_device->CreateFence(initialValue, flags, IID_PPV_ARGS(fence));
    }
#endif

    GraphicsRuntime GetBuildDefaultGraphicsRuntime()
    {
#if RHI_DIRECTX12
        return GraphicsRuntime::DirectX12;
#elif RHI_VULKAN
        return GraphicsRuntime::Vulkan;
#elif RHI_DIRECTX11
        return GraphicsRuntime::DirectX11;
#elif RHI_OPENGL
        return GraphicsRuntime::OpenGL;
#else
        return GraphicsRuntime::DirectX12;
#endif
    }

    bool IsGraphicsRuntimeEnabled(GraphicsRuntime runtime)
    {
        switch (runtime) {
        case GraphicsRuntime::DirectX12: return RHI_DIRECTX12 != 0;
        case GraphicsRuntime::Vulkan: return RHI_VULKAN != 0;
        case GraphicsRuntime::DirectX11: return RHI_DIRECTX11 != 0;
        case GraphicsRuntime::OpenGL: return RHI_OPENGL != 0;
        default: return false;
        }
    }

    std::unique_ptr<IRHIDevice> CreateRHIDevice(GraphicsRuntime runtime)
    {
        switch (runtime) {
        case GraphicsRuntime::DirectX12:
#if RHI_DIRECTX12
            return std::make_unique<Dx12GraphicsDevice>();
#else
            DebugLog("GraphicsRuntime::DirectX12 is disabled by build symbol.\n");
            return nullptr;
#endif
        case GraphicsRuntime::Vulkan:
#if RHI_VULKAN
            DebugLog("GraphicsRuntime::Vulkan is enabled but backend is not implemented yet.\n");
#else
            DebugLog("GraphicsRuntime::Vulkan is disabled by build symbol.\n");
#endif
            return nullptr;
        case GraphicsRuntime::DirectX11:
#if RHI_DIRECTX11
            DebugLog("GraphicsRuntime::DirectX11 is enabled but backend is not implemented yet.\n");
#else
            DebugLog("GraphicsRuntime::DirectX11 is disabled by build symbol.\n");
#endif
            return nullptr;
        case GraphicsRuntime::OpenGL:
#if RHI_OPENGL
            DebugLog("GraphicsRuntime::OpenGL is enabled but backend is not implemented yet.\n");
#else
            DebugLog("GraphicsRuntime::OpenGL is disabled by build symbol.\n");
#endif
            return nullptr;
        default:
            return nullptr;
        }
    }

    const char* GraphicsRuntimeToString(GraphicsRuntime runtime)
    {
        switch (runtime) {
        case GraphicsRuntime::DirectX12: return "DirectX12";
        case GraphicsRuntime::Vulkan: return "Vulkan";
        case GraphicsRuntime::DirectX11: return "DirectX11";
        case GraphicsRuntime::OpenGL: return "OpenGL";
        default: return "Unknown";
        }
    }
}
