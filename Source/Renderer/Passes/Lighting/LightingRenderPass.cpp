#include "Renderer/Passes/Lighting/LightingRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    void LightingRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
    }

    void LightingRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.Write("GBufferAlbedo");
        builder.Write("GBufferNormal");
        builder.Write("GBufferMaterial");
        builder.Write("GBufferEmissive");
        builder.UseColorTarget("SceneColor");
        builder.UseColorTarget("GBufferAlbedo");
        builder.UseColorTarget("GBufferNormal");
        builder.UseColorTarget("GBufferMaterial");
        builder.UseColorTarget("GBufferEmissive");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool LightingRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("LightingRenderPass::Execute: runtime context is invalid.\n");
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
        return true;
    }

    void LightingRenderPass::Execute(IRhiCommandEncoder* enc,
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

        if (useMeshletDebugView && pipelineStateCache.GetMeshletDebugPipelineState().Get()) {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetMeshletDebugPipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        } else if (useTessellation && useTessellationWireframe) {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationWireframePipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::PatchList);
        } else if (useTessellation) {
            if (useTessellationDebugColors && pipelineStateCache.GetTessellationDebugPipelineState().Get()) {
                enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationDebugPipelineState()));
            } else {
                enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationPipelineState()));
            }
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::PatchList);
        } else {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetPipelineState()));
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
