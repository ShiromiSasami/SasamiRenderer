#pragma once

#include "Renderer/RHI/RhiTypes.h"

#include <memory>

namespace SasamiRenderer
{
    class IRhiCommandEncoder
    {
    public:
        virtual ~IRhiCommandEncoder() = default;

        virtual void BeginDebugLabel(const char*) {}
        virtual void EndDebugLabel() {}
        virtual void TransitionResources(const RhiResourceTransitionDesc*, uint32_t) {}
        virtual void BeginRenderPass(const RhiRenderPassDesc&) {}
        virtual void EndRenderPass() {}
        virtual void SetGraphicsPipeline(RhiPipelineHandle) {}
        virtual void SetComputePipeline(RhiPipelineHandle) {}
        virtual void SetPrimitiveTopology(RhiPrimitiveTopology) {}
        virtual void SetViewports(const RhiViewport*, uint32_t) {}
        virtual void SetScissors(const RhiRect*, uint32_t) {}
        virtual void Draw(const RhiDrawDesc&) {}
        virtual void DrawIndexed(const RhiDrawIndexedDesc&) {}
        virtual void Dispatch(const RhiDispatchDesc&) {}
        virtual void CopyBuffer(const RhiBufferCopyDesc&) {}

        // Pipeline layout (root signature equivalent)
        virtual void SetGraphicsPipelineLayout(RhiPipelineLayoutHandle) {}
        virtual void SetComputePipelineLayout(RhiPipelineLayoutHandle) {}

        // Descriptor heap binding (DX12-specific; no-op on other backends)
        virtual void SetDescriptorHeap(RhiDescriptorHeapHandle) {}

        // Descriptor table binding
        virtual void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle) {}
        virtual void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle) {}

        // Inline root descriptors
        virtual void SetGraphicsConstantBufferView(uint32_t slot, RhiGpuAddress) {}
        virtual void SetComputeConstantBufferView(uint32_t slot, RhiGpuAddress) {}
        virtual void SetGraphicsShaderResourceView(uint32_t slot, RhiGpuAddress) {}
        virtual void SetComputeShaderResourceView(uint32_t slot, RhiGpuAddress) {}

        // Render targets / clear
        virtual void SetRenderTargets(uint32_t numRtvs,
                                      const RhiCpuDescriptorHandle* rtvs,
                                      const RhiCpuDescriptorHandle* dsv = nullptr) {}
        virtual void ClearRenderTarget(RhiCpuDescriptorHandle rtv, const RhiClearColor& color) {}
        virtual void ClearDepthStencil(RhiCpuDescriptorHandle dsv, float depth, uint8_t stencil) {}

        // Vertex / index buffers
        virtual void SetVertexBuffers(uint32_t startSlot, uint32_t count,
                                      const RhiVertexBufferView* views) {}
        virtual void SetIndexBuffer(const RhiIndexBufferView& view) {}
        virtual void SetVertexBufferBindings(uint32_t startSlot, uint32_t count,
                                             const RhiVertexBufferBinding* bindings) {}
        virtual void SetIndexBufferBinding(const RhiIndexBufferBinding& binding) {}

        // Ray tracing dispatch
        virtual void DispatchRays(const RhiDispatchRaysDesc&) {}
    };

    class NullRhiCommandEncoder final : public IRhiCommandEncoder
    {
    };

    class IRhiDevice
    {
    public:
        virtual ~IRhiDevice() = default;

        virtual RhiBackendApi GetRhiBackendApi() const = 0;
        virtual const RhiBackendCapabilities& GetRhiCapabilities() const = 0;
        virtual bool WaitForRhiIdle() = 0;
        virtual bool ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc) = 0;

        virtual RhiTextureHandle CreateRhiTexture(const RhiTextureDesc& desc) = 0;
        virtual RhiBufferHandle CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData = nullptr) = 0;
        virtual bool UpdateRhiBuffer(RhiBufferHandle buffer,
                                     uint64_t offsetInBytes,
                                     const void* data,
                                     uint64_t sizeInBytes) = 0;
        virtual bool ReadRhiBuffer(RhiBufferHandle buffer,
                                   uint64_t offsetInBytes,
                                   void* data,
                                   uint64_t sizeInBytes) = 0;
        virtual bool DestroyRhiResource(RhiResourceHandle resource) = 0;
        virtual RhiShaderHandle CreateRhiShaderModule(const RhiShaderModuleDesc& desc) = 0;
        virtual RhiPipelineLayoutHandle CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc) = 0;
        virtual RhiPipelineHandle CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) = 0;
        virtual RhiPipelineHandle CreateRhiComputePipeline(const RhiComputePipelineDesc& desc) = 0;
        // Create*Pipeline handles carry a backend-internal id (e.g. a map key). Command-encoder
        // binding, however, expects a handle whose id is the raw native object pointer (see
        // RenderPipelineStateCache::MakeLayoutHandle/MakePipelineHandle). These convert a created
        // handle into the encoder-bindable form. Default: identity (backends whose create handles
        // are already bindable, or which do not use these create paths).
        virtual RhiPipelineLayoutHandle GetBindablePipelineLayoutHandle(RhiPipelineLayoutHandle handle) { return handle; }
        virtual RhiPipelineHandle GetBindablePipelineHandle(RhiPipelineHandle handle) { return handle; }
        virtual RhiDescriptorAllocation AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                              uint32_t count,
                                                              bool shaderVisible) = 0;
        virtual bool CreateRhiShaderResourceView(RhiResourceHandle resource,
                                                 const RhiTextureViewDesc& desc,
                                                 RhiCpuDescriptorHandle destination) = 0;
        virtual bool CreateRhiBufferShaderResourceView(RhiBufferHandle buffer,
                                                       const RhiBufferViewDesc& desc,
                                                       RhiCpuDescriptorHandle destination) = 0;
        virtual bool CreateRhiUnorderedAccessView(RhiTextureHandle texture,
                                                  const RhiTextureViewDesc& desc,
                                                  RhiCpuDescriptorHandle destination) = 0;
        virtual bool CreateRhiBufferUnorderedAccessView(RhiBufferHandle buffer,
                                                        const RhiBufferViewDesc& desc,
                                                        RhiCpuDescriptorHandle destination) = 0;
        virtual bool CreateRhiRenderTargetView(RhiTextureHandle texture,
                                               const RhiRenderTargetViewDesc& desc,
                                               RhiCpuDescriptorHandle destination) = 0;
        virtual bool CreateRhiDepthStencilView(RhiTextureHandle texture,
                                               const RhiDepthStencilViewDesc& desc,
                                               RhiCpuDescriptorHandle destination) = 0;

        virtual std::unique_ptr<IRhiCommandEncoder> CreateCommandEncoder(RhiQueueType queueType) = 0;
        virtual bool SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType) = 0;

        // Ray tracing (default no-op; DX11/OpenGL inherit these empty implementations)
        virtual bool BuildRhiBlases(
            const RhiBlasDesc* descs, uint32_t count,
            RhiAccelerationStructureHandle* outHandles) { (void)descs; (void)count; (void)outHandles; return false; }
        virtual RhiAccelerationStructureHandle BuildRhiTlas(const RhiTlasDesc&)    { return {}; }
        virtual bool DestroyRhiAccelerationStructure(RhiAccelerationStructureHandle) { return false; }
        virtual RhiGpuAddress GetRhiAccelerationStructureGpuAddress(RhiAccelerationStructureHandle) { return 0; }
        virtual bool CreateRhiAccelerationStructureSrv(
            RhiAccelerationStructureHandle, RhiCpuDescriptorHandle) { return false; }
        virtual RhiRayTracingPipelineHandle CreateRhiRayTracingPipeline(
            const RhiRayTracingPipelineDesc&) { return {}; }
        virtual RhiShaderBindingTableHandle CreateRhiShaderBindingTable(
            const RhiShaderBindingTableDesc&) { return {}; }
    };
}
