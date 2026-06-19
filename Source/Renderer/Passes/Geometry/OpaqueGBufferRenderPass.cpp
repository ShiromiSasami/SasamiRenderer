#include "Renderer/Passes/Geometry/OpaqueGBufferRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    namespace
    {
        void ExecuteSkinnedOpaqueGBufferPass(IRhiCommandEncoder* enc,
                                             RenderPipelineStateCache& pipelineStateCache,
                                             DescriptorHeap& srvHeap,
                                             GpuDescriptorHandle shadowSrv,
                                             GpuDescriptorHandle spotShadowSrv,
                                             GpuDescriptorHandle vsmSrv,
                                             GpuDescriptorHandle lightSrvTable,
                                             GpuDescriptorHandle iblSrvTable,
                                             GpuDescriptorHandle aoSrv,
                                             GpuDescriptorHandle reflectionSrv,
                                             GpuDescriptorHandle transparentBackfaceDistanceSrv,
                                             D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                             const std::function<void()>& drawSkinnedCallback)
        {
            if (!enc || !drawSkinnedCallback) return;

            enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetSkinnedRootSignature()));
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetSkinnedGBufferPipelineState()));

            enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
            enc->SetGraphicsDescriptorTable(1,  { shadowSrv.ptr });
            enc->SetGraphicsDescriptorTable(4,  { lightSrvTable.ptr });
            enc->SetGraphicsDescriptorTable(5,  { iblSrvTable.ptr });
            enc->SetGraphicsDescriptorTable(6,  { aoSrv.ptr });
            enc->SetGraphicsDescriptorTable(7,  { reflectionSrv.ptr });
            enc->SetGraphicsDescriptorTable(12, { spotShadowSrv.ptr });
            enc->SetGraphicsDescriptorTable(13, { vsmSrv.ptr });
            enc->SetGraphicsDescriptorTable(14, { transparentBackfaceDistanceSrv.ptr });
            if (lightCbGpu != 0) {
                enc->SetGraphicsConstantBufferView(3, lightCbGpu);
            }
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

            drawSkinnedCallback();
        }
    }

    void OpaqueGBufferRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawOpaqueItems();
    }

    void OpaqueGBufferRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Write("SceneDepth");
        builder.Write("GBufferAlbedo");
        builder.Write("GBufferNormal");
        builder.Write("GBufferMaterial");
        builder.Write("GBufferEmissive");
        builder.Write("GBufferSpecularWorkflow");
        builder.UseColorTarget("GBufferAlbedo");
        builder.UseColorTarget("GBufferNormal");
        builder.UseColorTarget("GBufferMaterial");
        builder.UseColorTarget("GBufferEmissive");
        builder.UseColorTarget("GBufferSpecularWorkflow");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool OpaqueGBufferRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("OpaqueGBufferRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();
        if (!policy.executeLightingFamilyPasses) {
            return true;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        const RenderPassExecutionServices& services = context.Services();

        Execute(inputs.execution.commandEncoder,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                *inputs.execution.scissorRect,
                policy.useTessellation,
                policy.useTessellationWireframe,
                policy.useTessellationDebugColors,
                policy.useMeshletDebugView,
                inputs.shadow.shadowSrv,
                inputs.shadow.spotShadowSrv,
                inputs.shadow.vsmSrv,
                inputs.lighting.lightSrvTable,
                inputs.lighting.iblSrvTable,
                inputs.ao.aoSrv,
                inputs.reflectionSrv,
                inputs.gbuffer.depthSrv,
                inputs.transparentBackfaceDistanceSrv,
                inputs.lighting.lightCbGpu,
                services.drawOpaqueItems);

        if (services.drawSkinnedOpaqueItems) {
            ExecuteSkinnedOpaqueGBufferPass(inputs.execution.commandEncoder,
                                            *inputs.execution.pipelineStateCache,
                                            *inputs.execution.srvHeap,
                                            inputs.shadow.shadowSrv,
                                            inputs.shadow.spotShadowSrv,
                                            inputs.shadow.vsmSrv,
                                            inputs.lighting.lightSrvTable,
                                            inputs.lighting.iblSrvTable,
                                            inputs.ao.aoSrv,
                                            inputs.reflectionSrv,
                                            inputs.transparentBackfaceDistanceSrv,
                                            inputs.lighting.lightCbGpu,
                                            services.drawSkinnedOpaqueItems);
        }
        return true;
    }

    void OpaqueGBufferRenderPass::Execute(IRhiCommandEncoder* enc,
                                          RenderPipelineStateCache& pipelineStateCache,
                                          DescriptorHeap& srvHeap,
                                          const Viewport& viewport,
                                          const Rect& scissorRect,
                                          bool useTessellation,
                                          bool useTessellationWireframe,
                                          bool useTessellationDebugColors,
                                          bool useMeshletDebugView,
                                          GpuDescriptorHandle shadowSrv,
                                          GpuDescriptorHandle spotShadowSrv,
                                          GpuDescriptorHandle vsmSrv,
                                          GpuDescriptorHandle lightSrvTable,
                                          GpuDescriptorHandle iblSrvTable,
                                          GpuDescriptorHandle aoSrv,
                                          GpuDescriptorHandle reflectionSrv,
                                          GpuDescriptorHandle depthSrv,
                                          GpuDescriptorHandle transparentBackfaceDistanceSrv,
                                          D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                          const std::function<void()>& drawCallback) const
    {
        if (!enc) {
            return;
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);

        (void)useMeshletDebugView;
        if (useTessellation && useTessellationWireframe) {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationGBufferWireframePipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::PatchList);
        } else if (useTessellation) {
            (void)useTessellationDebugColors;
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationGBufferPipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::PatchList);
        } else {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetGBufferPipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        }

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
        enc->SetGraphicsDescriptorTable(1,  { shadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(4,  { lightSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(5,  { iblSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(6,  { aoSrv.ptr });
        enc->SetGraphicsDescriptorTable(7,  { reflectionSrv.ptr });
        enc->SetGraphicsDescriptorTable(11, { depthSrv.ptr });
        enc->SetGraphicsDescriptorTable(12, { spotShadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(13, { vsmSrv.ptr });
        enc->SetGraphicsDescriptorTable(14, { transparentBackfaceDistanceSrv.ptr });

        if (lightCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(3, lightCbGpu);
        }

        if (drawCallback) {
            drawCallback();
        }
    }
}
