#include "Renderer/Passes/Lighting/DeferredLightingRenderPass.h"

#include <cstdio>

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    void DeferredLightingRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
    }

    void DeferredLightingRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Read("SceneDepth");
        builder.Read("GBufferAlbedo");
        builder.Read("GBufferNormal");
        builder.Read("GBufferMaterial");
        builder.Read("GBufferEmissive");
        builder.Read("GBufferSpecularWorkflow");
        builder.Read("SSAOBlur");
        builder.Read("SSRReflection");
        builder.Read("SoftwareReflection");
        // Spot/point/VSM shadow maps are owned by LightSystem/ShadowMapManager and are not
        // imported into the render graph, so they can't be declared here; they remain an
        // implicit dependency (known, tracked issue).
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.DependsOnPrevious();
    }

    bool DeferredLightingRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("DeferredLightingRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();

        const RenderPassFrameInputs& inputs = context.Inputs();
        IRhiCommandEncoder* enc = inputs.execution.commandEncoder;
        RenderPipelineStateCache& pipelineStateCache = *inputs.execution.pipelineStateCache;

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetDeferredLightingRootSignature()));
        PipelineState& pipelineState =
            (policy.gBufferDebugView == RendererEnums::GBufferDebugView::FinalLit)
                ? pipelineStateCache.GetDeferredLightingPipelineState()
                : pipelineStateCache.GetGBufferDebugPipelineState();
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineState));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(*inputs.execution.srvHeap));
        enc->SetGraphicsDescriptorTable(0,  { inputs.gbuffer.albedoSrv.ptr });
        enc->SetGraphicsDescriptorTable(1,  { inputs.gbuffer.normalSrv.ptr });
        enc->SetGraphicsDescriptorTable(2,  { inputs.gbuffer.materialSrv.ptr });
        enc->SetGraphicsDescriptorTable(3,  { inputs.gbuffer.emissiveSrv.ptr });
        enc->SetGraphicsDescriptorTable(4,  { inputs.shadow.shadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(5,  { inputs.gbuffer.depthSrv.ptr });
        enc->SetGraphicsDescriptorTable(6,  { inputs.lighting.lightSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(7,  { inputs.lighting.iblSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(8,  { inputs.ao.screenSpaceAoSrv.ptr });
        enc->SetGraphicsDescriptorTable(9,  { inputs.reflectionSrv.ptr });
        enc->SetGraphicsDescriptorTable(10, { inputs.shadow.spotShadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(11, { inputs.shadow.vsmSrv.ptr });
        enc->SetGraphicsDescriptorTable(12, { inputs.transparentBackfaceDistanceSrv.ptr });
        enc->SetGraphicsDescriptorTable(13, { inputs.gbuffer.specularWorkflowSrv.ptr });
        if (inputs.lighting.lightCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(14, inputs.lighting.lightCbGpu);
        }
        if (inputs.lighting.giProbeGridCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(15, inputs.lighting.giProbeGridCbGpu);
        }
        if (inputs.lighting.giProbeDataGpu != 0) {
            enc->SetGraphicsShaderResourceView(16, inputs.lighting.giProbeDataGpu);
        }
        enc->SetGraphicsDescriptorTable(17, { inputs.shadow.pointShadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(18, { inputs.lighting.iblSpecularSrvTable.ptr });
        enc->Draw({ 3u, 1u, 0u, 0u });
        return true;
    }
}
