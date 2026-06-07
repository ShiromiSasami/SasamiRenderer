#pragma once

#include <memory>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "d3dx12.h"
#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/RHI/RhiTypes.h"

#ifndef PLATFORM_WINDOWS
#if defined(_WIN32)
#define PLATFORM_WINDOWS 1
#else
#define PLATFORM_WINDOWS 0
#endif
#endif
#ifndef PLATFORM_LINUX
#if defined(__linux__) && !defined(__ANDROID__)
#define PLATFORM_LINUX 1
#else
#define PLATFORM_LINUX 0
#endif
#endif
#ifndef PLATFORM_MACOS
#if defined(__APPLE__) && !defined(__ANDROID__)
#define PLATFORM_MACOS 1
#else
#define PLATFORM_MACOS 0
#endif
#endif
#ifndef PLATFORM_ANDROID
#if defined(__ANDROID__)
#define PLATFORM_ANDROID 1
#else
#define PLATFORM_ANDROID 0
#endif
#endif

// Backward compatibility: allow legacy graphics-macro names from build files.
#if defined(PLATFORM_DX12) && !defined(RHI_DIRECTX12)
#define RHI_DIRECTX12 PLATFORM_DX12
#endif
#if defined(PLATFORM_DX11) && !defined(RHI_DIRECTX11)
#define RHI_DIRECTX11 PLATFORM_DX11
#endif
#if defined(PLATFORM_VULKAN) && !defined(RHI_VULKAN)
#define RHI_VULKAN PLATFORM_VULKAN
#endif
#if defined(PLATFORM_OPENGL) && !defined(RHI_OPENGL)
#define RHI_OPENGL PLATFORM_OPENGL
#endif

#ifndef RHI_DIRECTX12
#define RHI_DIRECTX12 1
#endif
#ifndef RHI_DIRECTX11
#define RHI_DIRECTX11 0
#endif
#ifndef RHI_VULKAN
#define RHI_VULKAN 0
#endif
#ifndef RHI_OPENGL
#define RHI_OPENGL 0
#endif

#if !PLATFORM_WINDOWS && !PLATFORM_LINUX && !PLATFORM_MACOS && !PLATFORM_ANDROID
#error "At least one PLATFORM_* OS macro must be enabled."
#endif
#if !RHI_DIRECTX12 && !RHI_DIRECTX11 && !RHI_VULKAN && !RHI_OPENGL
#error "At least one RHI_* backend macro must be enabled."
#endif

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;
    enum class GraphicsRuntime;

    using Format = DXGI_FORMAT;
    using Viewport = D3D12_VIEWPORT;
    using Rect = D3D12_RECT;
    using CpuDescriptorHandle = D3D12_CPU_DESCRIPTOR_HANDLE;
    using GpuDescriptorHandle = D3D12_GPU_DESCRIPTOR_HANDLE;
    using ResourceDesc = D3D12_RESOURCE_DESC;
    using HeapProperties = D3D12_HEAP_PROPERTIES;
    using ClearValue = D3D12_CLEAR_VALUE;
    using ResourceState = D3D12_RESOURCE_STATES;
    using ResourceBarrier = D3D12_RESOURCE_BARRIER;
    using DescriptorHeapDesc = D3D12_DESCRIPTOR_HEAP_DESC;
    using DepthStencilViewDesc = D3D12_DEPTH_STENCIL_VIEW_DESC;
    using ShaderResourceViewDesc = D3D12_SHADER_RESOURCE_VIEW_DESC;
    using GraphicsPipelineDesc = D3D12_GRAPHICS_PIPELINE_STATE_DESC;
    using RootSignatureDesc = D3D12_ROOT_SIGNATURE_DESC;
    using RootParameter = D3D12_ROOT_PARAMETER;
    using DescriptorRange = D3D12_DESCRIPTOR_RANGE;
    using StaticSamplerDesc = D3D12_STATIC_SAMPLER_DESC;
    using InputElementDesc = D3D12_INPUT_ELEMENT_DESC;
    using SubresourceData = D3D12_SUBRESOURCE_DATA;
    using VertexBufferView = D3D12_VERTEX_BUFFER_VIEW;
    using IndexBufferView = D3D12_INDEX_BUFFER_VIEW;
    using ClearFlags = D3D12_CLEAR_FLAGS;
    using PrimitiveTopology = D3D12_PRIMITIVE_TOPOLOGY;
    using CommandListType = D3D12_COMMAND_LIST_TYPE;
    using DescriptorHeapType = D3D12_DESCRIPTOR_HEAP_TYPE;
    using DescriptorHeapFlags = D3D12_DESCRIPTOR_HEAP_FLAGS;
    using HeapFlags = D3D12_HEAP_FLAGS;

    class Resource
    {
    public:
        ID3D12Resource* Get() const { return m_resource.Get(); }
        ID3D12Resource* operator->() const { return m_resource.Get(); }
        ID3D12Resource** GetAddressOf() { return m_resource.GetAddressOf(); }
        void Attach(ID3D12Resource* resource) { m_resource = resource; }
        bool IsValid() const { return m_resource != nullptr; }
        void Reset() { m_resource.Reset(); }
        HRESULT Map(UINT subresource, const D3D12_RANGE* range, void** data) { return m_resource->Map(subresource, range, data); }
        void Unmap(UINT subresource, const D3D12_RANGE* range) { m_resource->Unmap(subresource, range); }
        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return m_resource->GetGPUVirtualAddress(); }

    private:
        ComPtr<ID3D12Resource> m_resource;
        friend class Dx12GraphicsDevice;
    };

    class DescriptorHeap
    {
    public:
        ID3D12DescriptorHeap* Get() const { return m_heap.Get(); }
        ID3D12DescriptorHeap* operator->() const { return m_heap.Get(); }
        CpuDescriptorHandle GetCPUDescriptorHandleForHeapStart() const { return m_heap->GetCPUDescriptorHandleForHeapStart(); }
        GpuDescriptorHandle GetGPUDescriptorHandleForHeapStart() const { return m_heap->GetGPUDescriptorHandleForHeapStart(); }
        void Reset() { m_heap.Reset(); }

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        friend class Dx12GraphicsDevice;
    };

    class CommandAllocator
    {
    public:
        ID3D12CommandAllocator* Get() const { return m_allocator.Get(); }
        ID3D12CommandAllocator* operator->() const { return m_allocator.Get(); }
        HRESULT Reset() { return m_allocator->Reset(); }

    private:
        ComPtr<ID3D12CommandAllocator> m_allocator;
        friend class Dx12GraphicsDevice;
    };

    class PipelineState
    {
    public:
        ID3D12PipelineState* Get() const { return m_state.Get(); }
        ID3D12PipelineState* operator->() const { return m_state.Get(); }

    private:
        ComPtr<ID3D12PipelineState> m_state;
        friend class Dx12GraphicsDevice;
    };

    class RootSignature
    {
    public:
        ID3D12RootSignature* Get() const { return m_signature.Get(); }
        ID3D12RootSignature* operator->() const { return m_signature.Get(); }

    private:
        ComPtr<ID3D12RootSignature> m_signature;
        friend class Dx12GraphicsDevice;
    };

    class CommandList
    {
    public:
        ID3D12GraphicsCommandList* Get() const { return m_list.Get(); }
        ID3D12GraphicsCommandList* operator->() const { return m_list.Get(); }

        HRESULT Reset(CommandAllocator& allocator, PipelineState* initialState = nullptr)
        {
            return m_list->Reset(allocator.Get(), initialState ? initialState->Get() : nullptr);
        }

        HRESULT Close() { return m_list->Close(); }

        void ResourceBarrier(UINT count, const ResourceBarrier* barriers) { m_list->ResourceBarrier(count, barriers); }
        void RSSetViewports(UINT count, const Viewport* viewports) { m_list->RSSetViewports(count, viewports); }
        void RSSetScissorRects(UINT count, const Rect* rects) { m_list->RSSetScissorRects(count, rects); }
        void OMSetRenderTargets(UINT num, const CpuDescriptorHandle* rtvs, BOOL singleHandle, const CpuDescriptorHandle* dsv)
        {
            m_list->OMSetRenderTargets(num, rtvs, singleHandle, dsv);
        }
        void ClearRenderTargetView(CpuDescriptorHandle rtv, const float color[4], UINT numRects, const Rect* rects)
        {
            m_list->ClearRenderTargetView(rtv, color, numRects, rects);
        }
        void ClearDepthStencilView(CpuDescriptorHandle dsv, ClearFlags flags, float depth, UINT8 stencil, UINT numRects, const Rect* rects)
        {
            m_list->ClearDepthStencilView(dsv, flags, depth, stencil, numRects, rects);
        }
        void SetPipelineState(PipelineState& state) { m_list->SetPipelineState(state.Get()); }
        void SetPipelineState(ID3D12PipelineState* state) { m_list->SetPipelineState(state); }
        void SetGraphicsRootSignature(RootSignature& sig) { m_list->SetGraphicsRootSignature(sig.Get()); }
        void SetComputeRootSignature(RootSignature& sig) { m_list->SetComputeRootSignature(sig.Get()); }
        void SetComputeRootSignature(ID3D12RootSignature* sig) { m_list->SetComputeRootSignature(sig); }
        void SetDescriptorHeaps(UINT count, DescriptorHeap* const* heaps)
        {
            std::vector<ID3D12DescriptorHeap*> native;
            native.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                native.push_back(heaps[i] ? heaps[i]->Get() : nullptr);
            }
            m_list->SetDescriptorHeaps(count, native.data());
        }
        void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps)
        {
            m_list->SetDescriptorHeaps(count, heaps);
        }
        void SetGraphicsRootDescriptorTable(UINT rootIndex, GpuDescriptorHandle handle)
        {
            m_list->SetGraphicsRootDescriptorTable(rootIndex, handle);
        }
        void SetGraphicsRootConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
        {
            m_list->SetGraphicsRootConstantBufferView(rootIndex, address);
        }
        void SetGraphicsRootShaderResourceView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
        {
            m_list->SetGraphicsRootShaderResourceView(rootIndex, address);
        }
        void SetComputeRoot32BitConstants(UINT rootIndex, UINT num32BitValues, const void* data, UINT destOffset)
        {
            m_list->SetComputeRoot32BitConstants(rootIndex, num32BitValues, data, destOffset);
        }
        void SetComputeRootDescriptorTable(UINT rootIndex, GpuDescriptorHandle handle)
        {
            m_list->SetComputeRootDescriptorTable(rootIndex, handle);
        }
        void SetComputeRootConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
        {
            m_list->SetComputeRootConstantBufferView(rootIndex, address);
        }
        void SetComputeRootShaderResourceView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
        {
            m_list->SetComputeRootShaderResourceView(rootIndex, address);
        }
        void SetComputeRootUnorderedAccessView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
        {
            m_list->SetComputeRootUnorderedAccessView(rootIndex, address);
        }
        void IASetPrimitiveTopology(PrimitiveTopology topology) { m_list->IASetPrimitiveTopology(topology); }
        void IASetVertexBuffers(UINT startSlot, UINT numViews, const VertexBufferView* views)
        {
            m_list->IASetVertexBuffers(startSlot, numViews, views);
        }
        void IASetIndexBuffer(const IndexBufferView* view) { m_list->IASetIndexBuffer(view); }
        void DrawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance)
        {
            m_list->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
        }
        void DrawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance)
        {
            m_list->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
        }
        void Dispatch(UINT threadGroupCountX, UINT threadGroupCountY, UINT threadGroupCountZ)
        {
            m_list->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
        }
        void CopyResource(Resource& dst, Resource& src) { m_list->CopyResource(dst.Get(), src.Get()); }
        void CopyBufferRegion(Resource& dst, UINT64 dstOffset, Resource& src, UINT64 srcOffset, UINT64 numBytes)
        {
            m_list->CopyBufferRegion(dst.Get(), dstOffset, src.Get(), srcOffset, numBytes);
        }

    private:
        ComPtr<ID3D12GraphicsCommandList> m_list;
        friend class Dx12GraphicsDevice;
    };

    enum class CommandQueueType { Graphics, Compute };

    class CommandQueue
    {
    public:
        ID3D12CommandQueue* Get() const { return m_queue.Get(); }
        ID3D12CommandQueue* operator->() const { return m_queue.Get(); }
        void ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists)
        {
            m_queue->ExecuteCommandLists(count, lists);
        }
        HRESULT Signal(ID3D12Fence* fence, UINT64 value) { return m_queue->Signal(fence, value); }
        HRESULT Wait(ID3D12Fence* fence, UINT64 value)   { return m_queue->Wait(fence, value);   }
        bool IsValid() const { return m_queue != nullptr; }

    private:
        ComPtr<ID3D12CommandQueue> m_queue;
        friend class Dx12GraphicsDevice;
    };

    class SwapChain
    {
    public:
        IDXGISwapChain3* Get() const { return m_swapChain.Get(); }
        IDXGISwapChain3* operator->() const { return m_swapChain.Get(); }
        UINT GetCurrentBackBufferIndex() const { return m_swapChain->GetCurrentBackBufferIndex(); }
        HRESULT ResizeBuffers(UINT bufferCount, UINT width, UINT height, Format format, UINT flags)
        {
            return m_swapChain->ResizeBuffers(bufferCount, width, height, format, flags);
        }
        HRESULT Present(UINT syncInterval, UINT flags) { return m_swapChain->Present(syncInterval, flags); }
        HRESULT GetBuffer(UINT index, REFIID riid, void** out) { return m_swapChain->GetBuffer(index, riid, out); }

    private:
        ComPtr<IDXGISwapChain3> m_swapChain;
        friend class Dx12GraphicsDevice;
    };

    class IRHIDevice : public IRhiDevice
    {
    public:
        virtual ~IRHIDevice() = default;

        virtual bool Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount = 2) = 0;
        virtual GraphicsRuntime GetBackend() const = 0;
        virtual void* GetNativeDeviceHandle() const = 0;
        virtual void* GetNativeGraphicsQueueHandle() const = 0;
        virtual ID3D12Device* GetDevice() const = 0;
        virtual ID3D12Device5* GetRayTracingDevice() const = 0;
        virtual const RhiBackendCapabilities& GetCapabilities() const = 0;
        virtual bool SupportsHardwareRayTracing() const = 0;
        virtual CommandQueue& GetCommandQueue() = 0;
        virtual CommandQueue& GetComputeQueue() = 0;
        virtual SwapChain& GetSwapChain() = 0;
        virtual UINT GetDescriptorHandleIncrementSize(DescriptorHeapType type) const = 0;
        virtual void WaitForGPU() = 0;
        RhiBackendApi GetRhiBackendApi() const override { return GetCapabilities().api; }
        const RhiBackendCapabilities& GetRhiCapabilities() const override { return GetCapabilities(); }
        bool WaitForRhiIdle() override
        {
            WaitForGPU();
            return true;
        }
        virtual bool RenderBackendClearFrame(const float clearColor[4])
        {
            (void)clearColor;
            return false;
        }
        virtual bool ResizeBackendSwapChain(UINT width, UINT height)
        {
            (void)width;
            (void)height;
            return false;
        }
        virtual RhiTextureHandle CreateRhiTexture2DFromRgba8(uint32_t width,
                                                             uint32_t height,
                                                             const void* pixels,
                                                             uint32_t rowPitchBytes)
        {
            (void)width;
            (void)height;
            (void)pixels;
            (void)rowPitchBytes;
            return {};
        }
        bool ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc) override
        {
            const float clearColor[] = {
                frameDesc.clearColor.r,
                frameDesc.clearColor.g,
                frameDesc.clearColor.b,
                frameDesc.clearColor.a,
            };
            return RenderBackendClearFrame(clearColor);
        }

        RhiTextureHandle CreateRhiTexture(const RhiTextureDesc& desc) override
        {
            (void)desc;
            return {};
        }
        RhiBufferHandle CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData = nullptr) override
        {
            (void)desc;
            (void)initialData;
            return {};
        }
        RhiShaderHandle CreateRhiShaderModule(const RhiShaderModuleDesc& desc) override
        {
            (void)desc;
            return {};
        }
        RhiPipelineLayoutHandle CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc) override
        {
            (void)desc;
            return {};
        }
        RhiPipelineHandle CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) override
        {
            (void)desc;
            return {};
        }
        RhiPipelineHandle CreateRhiComputePipeline(const RhiComputePipelineDesc& desc) override
        {
            (void)desc;
            return {};
        }
        RhiDescriptorAllocation AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                      uint32_t count,
                                                      bool shaderVisible) override
        {
            (void)type;
            (void)count;
            (void)shaderVisible;
            return {};
        }
        bool CreateRhiShaderResourceView(RhiResourceHandle resource,
                                         const RhiTextureViewDesc& desc,
                                         RhiCpuDescriptorHandle destination) override
        {
            (void)resource;
            (void)desc;
            (void)destination;
            return false;
        }
        bool CreateRhiRenderTargetView(RhiTextureHandle texture,
                                       const RhiRenderTargetViewDesc& desc,
                                       RhiCpuDescriptorHandle destination) override
        {
            (void)texture;
            (void)desc;
            (void)destination;
            return false;
        }
        bool CreateRhiDepthStencilView(RhiTextureHandle texture,
                                       const RhiDepthStencilViewDesc& desc,
                                       RhiCpuDescriptorHandle destination) override
        {
            (void)texture;
            (void)desc;
            (void)destination;
            return false;
        }
        virtual Resource* GetD3D12CompatibilityResource(RhiResourceHandle handle)
        {
            (void)handle;
            return nullptr;
        }
        virtual PipelineState* GetD3D12CompatibilityPipelineState(RhiPipelineHandle handle)
        {
            (void)handle;
            return nullptr;
        }
        std::unique_ptr<IRhiCommandEncoder> CreateCommandEncoder(RhiQueueType queueType) override
        {
            (void)queueType;
            return std::make_unique<NullRhiCommandEncoder>();
        }
        bool SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType) override
        {
            (void)encoder;
            (void)queueType;
            return false;
        }
        virtual bool BeginRhiRenderPass(const RhiRenderPassDesc& desc)
        {
            (void)desc;
            return false;
        }
        virtual void EndRhiRenderPass()
        {
        }

        // Compatibility surface for existing D3D12-backed renderer code. New
        // cross-backend code should use the neutral Rhi* descriptors above.
        virtual HRESULT CreateDescriptorHeap(const DescriptorHeapDesc& desc, DescriptorHeap& out) = 0;
        virtual HRESULT CreateCommittedResource(const HeapProperties* heapProps, HeapFlags heapFlags, const ResourceDesc* desc,
                                                ResourceState initialState, const ClearValue* clearValue, Resource& out) = 0;
        virtual HRESULT CreateCommandAllocator(CommandListType type, CommandAllocator& out) = 0;
        virtual HRESULT CreateCommandList(UINT nodeMask, CommandListType type, CommandAllocator& allocator,
                                          PipelineState* initialPSO, CommandList& out) = 0;
        virtual HRESULT CreateGraphicsPipelineState(const GraphicsPipelineDesc& desc, PipelineState& out) = 0;
        virtual HRESULT CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, PipelineState& out) = 0;
        virtual HRESULT CreatePipelineStateFromStream(const void* streamData, size_t streamSize, PipelineState& out) = 0;
        virtual HRESULT CreateRootSignature(UINT nodeMask, const void* blobData, size_t blobSize, RootSignature& out) = 0;
        virtual void CreateShaderResourceView(Resource& resource, const ShaderResourceViewDesc* desc, CpuDescriptorHandle dest) = 0;
        virtual void CreateDepthStencilView(Resource& resource, const DepthStencilViewDesc* desc, CpuDescriptorHandle dest) = 0;
        virtual void CreateRenderTargetView(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, CpuDescriptorHandle dest) = 0;
        virtual void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, CpuDescriptorHandle dest) = 0;
        virtual void CreateSampler(const D3D12_SAMPLER_DESC* desc, CpuDescriptorHandle dest) = 0;
        virtual HRESULT CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags, ID3D12Fence** fence) = 0;
    };

    enum class GraphicsRuntime
    {
        DirectX12,
        Vulkan,
        DirectX11,
        OpenGL,
    };

    // Runtime-facing API.
    GraphicsRuntime GetBuildDefaultGraphicsRuntime();
    bool IsGraphicsRuntimeEnabled(GraphicsRuntime runtime);
    std::unique_ptr<IRHIDevice> CreateRHIDevice(GraphicsRuntime runtime);
    const char* GraphicsRuntimeToString(GraphicsRuntime runtime);

    // Backward compatibility aliases during migration.
    using RHIBackend = GraphicsRuntime;

    inline RHIBackend GetBuildDefaultRHIBackend() { return GetBuildDefaultGraphicsRuntime(); }
    inline bool IsRHIBackendEnabled(RHIBackend backend) { return IsGraphicsRuntimeEnabled(backend); }
    inline const char* RHIBackendToString(RHIBackend backend) { return GraphicsRuntimeToString(backend); }

    // Backward compatibility aliases during migration.
    using IRHIResource = Resource;
    using IRHIDescriptorHeap = DescriptorHeap;
    using IRHICommandAllocator = CommandAllocator;
    using IRHICommandList = CommandList;
    using IRHICommandQueue = CommandQueue;
    using IRHISwapChain = SwapChain;

    using GraphicsDevice = IRHIDevice;
    using GraphicsBackend = GraphicsRuntime;

    inline GraphicsBackend GetBuildDefaultGraphicsBackend() { return GetBuildDefaultGraphicsRuntime(); }
    inline bool IsGraphicsBackendEnabled(GraphicsBackend backend) { return IsGraphicsRuntimeEnabled(backend); }
    inline std::unique_ptr<GraphicsDevice> CreateGraphicsDevice(GraphicsBackend backend) { return CreateRHIDevice(backend); }
    inline const char* GraphicsBackendToString(GraphicsBackend backend) { return GraphicsRuntimeToString(backend); }

    inline ResourceBarrier Transition(Resource& resource, ResourceState before, ResourceState after)
    {
        return CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), before, after);
    }
}
