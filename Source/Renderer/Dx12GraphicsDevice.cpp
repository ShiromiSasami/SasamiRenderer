#include "GraphicsDevice.h"
#include <windows.h>
#include <debugapi.h>

namespace SasamiRenderer
{
#if RHI_DIRECTX12
    class Dx12GraphicsDevice final : public IRHIDevice
    {
    public:
        ~Dx12GraphicsDevice() override
        {
            if (m_fenceEvent) {
                CloseHandle(m_fenceEvent);
                m_fenceEvent = nullptr;
            }
        }

        bool Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount) override
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
                m_device.Reset();
            };

            ComPtr<IDXGIFactory6> factory;
            HRESULT result = CreateDXGIFactory(IID_PPV_ARGS(&factory));
            if (FAILED(result)) {
                cleanup();
                return false;
            }

            result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device));
            if (FAILED(result)) {
                cleanup();
                return false;
            }

            D3D12_COMMAND_QUEUE_DESC cqDesc = {};
            cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            result = m_device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&m_commandQueue.m_queue));
            if (FAILED(result)) {
                cleanup();
                return false;
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
                swapChain1.Reset();
                cleanup();
                return false;
            }
            swapChain1.As(&m_swapChain.m_swapChain);

            result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
            if (FAILED(result)) { cleanup(); return false; }
            m_fenceValue = 0;
            m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!m_fenceEvent) { cleanup(); return false; }

            return true;
        }

        GraphicsRuntime GetBackend() const override { return GraphicsRuntime::DirectX12; }
        void* GetNativeDeviceHandle() const override { return m_device.Get(); }
        void* GetNativeGraphicsQueueHandle() const override { return m_commandQueue.m_queue.Get(); }
        ID3D12Device* GetDevice() const override { return m_device.Get(); }
        CommandQueue& GetCommandQueue() override { return m_commandQueue; }
        SwapChain& GetSwapChain() override { return m_swapChain; }
        UINT GetDescriptorHandleIncrementSize(DescriptorHeapType type) const override
        {
            return m_device->GetDescriptorHandleIncrementSize(type);
        }

        void WaitForGPU() override
        {
            if (!m_commandQueue.m_queue || !m_fence) return;
            const UINT64 v = ++m_fenceValue;
            m_commandQueue.m_queue->Signal(m_fence.Get(), v);
            if (m_fence->GetCompletedValue() < v) {
                m_fence->SetEventOnCompletion(v, m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, INFINITE);
            }
        }

        HRESULT CreateDescriptorHeap(const DescriptorHeapDesc& desc, DescriptorHeap& out) override
        {
            return m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out.m_heap));
        }

        HRESULT CreateCommittedResource(const HeapProperties* heapProps, HeapFlags heapFlags, const ResourceDesc* desc,
                                        ResourceState initialState, const ClearValue* clearValue, Resource& out) override
        {
            return m_device->CreateCommittedResource(heapProps, heapFlags, desc, initialState, clearValue, IID_PPV_ARGS(&out.m_resource));
        }

        HRESULT CreateCommandAllocator(CommandListType type, CommandAllocator& out) override
        {
            return m_device->CreateCommandAllocator(type, IID_PPV_ARGS(&out.m_allocator));
        }

        HRESULT CreateCommandList(UINT nodeMask, CommandListType type, CommandAllocator& allocator,
                                  PipelineState* initialPSO, CommandList& out) override
        {
            return m_device->CreateCommandList(nodeMask, type, allocator.Get(), initialPSO ? initialPSO->Get() : nullptr,
                                                IID_PPV_ARGS(&out.m_list));
        }

        HRESULT CreateGraphicsPipelineState(const GraphicsPipelineDesc& desc, PipelineState& out) override
        {
            return m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out.m_state));
        }

        HRESULT CreateRootSignature(UINT nodeMask, const void* blobData, size_t blobSize, RootSignature& out) override
        {
            return m_device->CreateRootSignature(nodeMask, blobData, blobSize, IID_PPV_ARGS(&out.m_signature));
        }

        void CreateShaderResourceView(Resource& resource, const ShaderResourceViewDesc* desc, CpuDescriptorHandle dest) override
        {
            m_device->CreateShaderResourceView(resource.Get(), desc, dest);
        }

        void CreateDepthStencilView(Resource& resource, const DepthStencilViewDesc* desc, CpuDescriptorHandle dest) override
        {
            m_device->CreateDepthStencilView(resource.Get(), desc, dest);
        }

        void CreateRenderTargetView(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, CpuDescriptorHandle dest) override
        {
            m_device->CreateRenderTargetView(resource.Get(), desc, dest);
        }

        void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, CpuDescriptorHandle dest) override
        {
            m_device->CreateConstantBufferView(desc, dest);
        }

        void CreateSampler(const D3D12_SAMPLER_DESC* desc, CpuDescriptorHandle dest) override
        {
            m_device->CreateSampler(desc, dest);
        }

        HRESULT CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags, ID3D12Fence** fence) override
        {
            return m_device->CreateFence(initialValue, flags, IID_PPV_ARGS(fence));
        }

    private:
        ComPtr<ID3D12Device> m_device;
        CommandQueue m_commandQueue;
        SwapChain m_swapChain;
        UINT m_bufferCount = 0;
        ComPtr<ID3D12Fence> m_fence;
        UINT64 m_fenceValue = 0;
        HANDLE m_fenceEvent = nullptr;
    };
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
            OutputDebugStringA("GraphicsRuntime::DirectX12 is disabled by build symbol.\n");
            return nullptr;
#endif
        case GraphicsRuntime::Vulkan:
#if RHI_VULKAN
            OutputDebugStringA("GraphicsRuntime::Vulkan is enabled but backend is not implemented yet.\n");
#else
            OutputDebugStringA("GraphicsRuntime::Vulkan is disabled by build symbol.\n");
#endif
            return nullptr;
        case GraphicsRuntime::DirectX11:
#if RHI_DIRECTX11
            OutputDebugStringA("GraphicsRuntime::DirectX11 is enabled but backend is not implemented yet.\n");
#else
            OutputDebugStringA("GraphicsRuntime::DirectX11 is disabled by build symbol.\n");
#endif
            return nullptr;
        case GraphicsRuntime::OpenGL:
#if RHI_OPENGL
            OutputDebugStringA("GraphicsRuntime::OpenGL is enabled but backend is not implemented yet.\n");
#else
            OutputDebugStringA("GraphicsRuntime::OpenGL is disabled by build symbol.\n");
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
