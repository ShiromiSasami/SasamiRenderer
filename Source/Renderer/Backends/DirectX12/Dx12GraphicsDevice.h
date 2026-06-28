#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstddef>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace SasamiRenderer
{
#if RHI_DIRECTX12
    class Dx12GraphicsDevice final : public IRHIDevice
    {
    public:
        ~Dx12GraphicsDevice() override;

        bool Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount = 2) override;
        GraphicsRuntime GetBackend() const override;
        void* GetNativeDeviceHandle() const override;
        void* GetNativeGraphicsQueueHandle() const override;
        ID3D12Device* GetDevice() const override;
        ID3D12Device5* GetRayTracingDevice() const override;
        const RhiBackendCapabilities& GetCapabilities() const override;
        bool SupportsHardwareRayTracing() const override;
        CommandQueue& GetCommandQueue() override;
        CommandQueue& GetComputeQueue() override;
        SwapChain& GetSwapChain() override;
        UINT GetDescriptorHandleIncrementSize(DescriptorHeapType type) const override;
        void WaitForGPU() override;
        RhiTextureHandle CreateRhiTexture2DFromRgba8(uint32_t width,
                                                     uint32_t height,
                                                     const void* pixels,
                                                     uint32_t rowPitchBytes) override;
        RhiTextureHandle CreateRhiTexture(const RhiTextureDesc& desc) override;
        RhiBufferHandle CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData = nullptr) override;
        bool UpdateRhiBuffer(RhiBufferHandle buffer,
                             uint64_t offsetInBytes,
                             const void* data,
                             uint64_t sizeInBytes) override;
        bool ReadRhiBuffer(RhiBufferHandle buffer,
                           uint64_t offsetInBytes,
                           void* data,
                           uint64_t sizeInBytes) override;
        bool DestroyRhiResource(RhiResourceHandle resource) override;
        RhiShaderHandle CreateRhiShaderModule(const RhiShaderModuleDesc& desc) override;
        RhiPipelineLayoutHandle CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc) override;
        RhiPipelineHandle CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) override;
        RhiPipelineHandle CreateRhiComputePipeline(const RhiComputePipelineDesc& desc) override;
        RhiDescriptorAllocation AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                      uint32_t count,
                                                      bool shaderVisible) override;
        bool CreateRhiShaderResourceView(RhiResourceHandle resource,
                                         const RhiTextureViewDesc& desc,
                                         RhiCpuDescriptorHandle destination) override;
        bool CreateRhiBufferShaderResourceView(RhiBufferHandle buffer,
                                               const RhiBufferViewDesc& desc,
                                               RhiCpuDescriptorHandle destination) override;
        bool CreateRhiUnorderedAccessView(RhiTextureHandle texture,
                                          const RhiTextureViewDesc& desc,
                                          RhiCpuDescriptorHandle destination) override;
        bool CreateRhiBufferUnorderedAccessView(RhiBufferHandle buffer,
                                                const RhiBufferViewDesc& desc,
                                                RhiCpuDescriptorHandle destination) override;
        bool CreateRhiRenderTargetView(RhiTextureHandle texture,
                                       const RhiRenderTargetViewDesc& desc,
                                       RhiCpuDescriptorHandle destination) override;
        bool CreateRhiDepthStencilView(RhiTextureHandle texture,
                                       const RhiDepthStencilViewDesc& desc,
                                       RhiCpuDescriptorHandle destination) override;
        std::unique_ptr<IRhiCommandEncoder> CreateCommandEncoder(RhiQueueType queueType) override;
        bool SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType) override;
        Resource* GetD3D12CompatibilityResource(RhiResourceHandle handle) override;

        // Ray tracing
        bool BuildRhiBlases(const RhiBlasDesc* descs, uint32_t count,
                             RhiAccelerationStructureHandle* outHandles) override;
        RhiAccelerationStructureHandle BuildRhiTlas(const RhiTlasDesc& desc) override;
        bool DestroyRhiAccelerationStructure(RhiAccelerationStructureHandle handle) override;
        RhiGpuAddress GetRhiAccelerationStructureGpuAddress(RhiAccelerationStructureHandle handle) override;
        bool CreateRhiAccelerationStructureSrv(
            RhiAccelerationStructureHandle handle, RhiCpuDescriptorHandle dest) override;
        RhiRayTracingPipelineHandle CreateRhiRayTracingPipeline(
            const RhiRayTracingPipelineDesc& desc) override;
        RhiShaderBindingTableHandle CreateRhiShaderBindingTable(
            const RhiShaderBindingTableDesc& desc) override;
        ID3D12StateObject*   GetDx12RayTracingStateObject(RhiRayTracingPipelineHandle handle) override;
        ID3D12RootSignature* GetDx12RayTracingRootSignature(RhiRayTracingPipelineHandle handle) override;
        bool FillDx12DispatchRaysDesc(RhiShaderBindingTableHandle handle, D3D12_DISPATCH_RAYS_DESC& outDesc) override;
        PipelineState* GetD3D12CompatibilityPipelineState(RhiPipelineHandle handle) override;

        HRESULT CreateDescriptorHeap(const DescriptorHeapDesc& desc, DescriptorHeap& out) override;
        HRESULT CreateCommittedResource(const HeapProperties* heapProps,
                                        HeapFlags heapFlags,
                                        const ResourceDesc* desc,
                                        ResourceState initialState,
                                        const ClearValue* clearValue,
                                        Resource& out) override;
        HRESULT CreateCommandAllocator(CommandListType type, CommandAllocator& out) override;
        HRESULT CreateCommandList(UINT nodeMask,
                                  CommandListType type,
                                  CommandAllocator& allocator,
                                  PipelineState* initialPSO,
                                  CommandList& out) override;
        HRESULT CreateGraphicsPipelineState(const GraphicsPipelineDesc& desc, PipelineState& out) override;
        HRESULT CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, PipelineState& out) override;
        HRESULT CreatePipelineStateFromStream(const void* streamData, size_t streamSize, PipelineState& out) override;
        HRESULT CreateRootSignature(UINT nodeMask, const void* blobData, size_t blobSize, RootSignature& out) override;
        void CreateShaderResourceView(Resource& resource, const ShaderResourceViewDesc* desc, CpuDescriptorHandle dest) override;
        void CreateDepthStencilView(Resource& resource, const DepthStencilViewDesc* desc, CpuDescriptorHandle dest) override;
        void CreateRenderTargetView(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, CpuDescriptorHandle dest) override;
        void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, CpuDescriptorHandle dest) override;
        void CreateSampler(const D3D12_SAMPLER_DESC* desc, CpuDescriptorHandle dest) override;
        HRESULT CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags, ID3D12Fence** fence) override;

    private:
        struct RhiShaderModule
        {
            RhiShaderStageFlags stage = RhiShaderStageFlags::None;
            std::vector<uint8_t> bytecode;
        };

        Resource* FindRhiResource(RhiResourceHandle handle);
        const Resource* FindRhiResource(RhiResourceHandle handle) const;
        RhiResourceHandle StoreRhiResource(Resource&& resource);
        PipelineState* FindRhiPipeline(RhiPipelineHandle handle);

        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12Device5> m_rayTracingDevice;
        CommandQueue m_commandQueue;
        CommandQueue m_computeQueue;
        SwapChain m_swapChain;
        UINT m_bufferCount = 0;
        ComPtr<ID3D12Fence> m_fence;
        UINT64 m_fenceValue = 0;
        HANDLE m_fenceEvent = nullptr;
        bool m_supportsHardwareRayTracing = false;
        RhiBackendCapabilities m_capabilities;
        uint64_t m_nextRhiResourceHandle = 1;
        uint64_t m_nextRhiShaderHandle = 1;
        uint64_t m_nextRhiPipelineLayoutHandle = 1;
        uint64_t m_nextRhiPipelineHandle = 1;
        uint64_t m_nextRhiAccelStructHandle = 1;
        uint64_t m_nextRhiRTPipelineHandle = 1;
        uint64_t m_nextRhiSBTHandle = 1;
        std::unordered_map<uint64_t, Resource> m_rhiResources;
        std::unordered_map<uint64_t, RhiShaderModule> m_rhiShaders;
        std::unordered_map<uint64_t, RootSignature> m_rhiPipelineLayouts;
        std::unordered_map<uint64_t, PipelineState> m_rhiPipelines;
        std::vector<DescriptorHeap> m_rhiDescriptorHeaps;

        // Ray tracing
        struct Dx12AccelStruct { Resource buffer; };
        struct Dx12RTPipeline {
            ComPtr<ID3D12StateObject>           stateObject;
            ComPtr<ID3D12StateObjectProperties> props;
            ComPtr<ID3D12RootSignature>         rootSignature;
            // Per-group export name (for SBT identifier lookup via GetShaderIdentifier)
            std::vector<std::wstring>           groupExportNames;
            uint32_t shaderGroupCount = 0;
        };
        struct Dx12SBT {
            Resource rayGenTable;
            Resource missTable;
            Resource hitGroupTable;
            UINT     rayGenRecordSize   = 0;
            UINT     missRecordSize     = 0;
            UINT     hitGroupRecordSize = 0;
            UINT     missCount          = 0;
            UINT     hitGroupCount      = 0;
        };
        std::unordered_map<uint64_t, Dx12AccelStruct> m_dx12AccelStructures;
        std::unordered_map<uint64_t, Dx12RTPipeline>  m_dx12RTPipelines;
        std::unordered_map<uint64_t, Dx12SBT>         m_dx12SBTs;
    };
#endif
}
