#include "Renderer/Passes/TransparentCompositeRenderNode.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Core/RenderGraph.h"

namespace SasamiRenderer
{
    void TransparentCompositeRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
    }

    void TransparentCompositeRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("TransparentOitAccum");
        builder.Read("TransparentOitRevealage");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.DependsOnPrevious();
    }

    bool TransparentCompositeRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentCompositeRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderNodeExecutionPolicy& policy = context.Policy();
        if (!policy.executeLightingFamilyPasses) {
            return true;
        }

        const RenderNodeFrameInputs& inputs = context.Inputs();
        IRhiCommandEncoder* enc = inputs.execution.commandEncoder;
        RenderPipelineStateCache& pipelineStateCache = *inputs.execution.pipelineStateCache;

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTransparentOitCompositePipelineState()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(*inputs.execution.srvHeap));
        enc->SetGraphicsDescriptorTable(0, { inputs.transmissionSceneColorSrv.ptr });
        enc->SetGraphicsDescriptorTable(6, { inputs.transparentOitAccumSrv.ptr });
        enc->SetGraphicsDescriptorTable(7, { inputs.transparentOitRevealageSrv.ptr });
        enc->Draw({ 3u, 1u, 0u, 0u });
        return true;
    }
}
