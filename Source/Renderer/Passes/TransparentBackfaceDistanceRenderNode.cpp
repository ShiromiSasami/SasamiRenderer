#include "Renderer/Passes/TransparentBackfaceDistanceRenderNode.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Core/RenderGraph.h"

namespace SasamiRenderer
{
    void TransparentBackfaceDistanceRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawTransparentItems();
    }

    void TransparentBackfaceDistanceRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneDepth");
        builder.Write("TransparentBackfaceDistance");
        builder.UseColorTarget("TransparentBackfaceDistance");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool TransparentBackfaceDistanceRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentBackfaceDistanceRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderNodeExecutionPolicy& policy = context.Policy();
        if (policy.renderPathMode != RendererEnums::RenderPathMode::Raster ||
            (!policy.executeOpaqueFamilyPasses && !policy.executeLightingFamilyPasses)) {
            return true;
        }

        const RenderNodeFrameInputs& inputs = context.Inputs();
        const RenderNodeExecutionServices& services = context.Services();
        Execute(inputs.execution.commandEncoder,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                *inputs.execution.scissorRect,
                inputs.lighting.lightCbGpu,
                services.drawTransparentItems,
                services.drawSkinnedTransparentItems);
        return true;
    }

    void TransparentBackfaceDistanceRenderNode::Execute(IRhiCommandEncoder* enc,
                                                        RenderPipelineStateCache& pipelineStateCache,
                                                        DescriptorHeap& srvHeap,
                                                        const Viewport& viewport,
                                                        const Rect& scissorRect,
                                                        D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                        const std::function<void()>& drawCallback,
                                                        const std::function<void()>& drawSkinnedCallback) const
    {
        if (!enc) {
            return;
        }

        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);
        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTransparentBackfaceDistancePipelineState()));
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        if (lightCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(3, lightCbGpu);
        }
        if (drawCallback) {
            drawCallback();
        }

        if (drawSkinnedCallback) {
            enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetSkinnedRootSignature()));
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetSkinnedTransparentBackfaceDistancePipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
            if (lightCbGpu != 0) {
                enc->SetGraphicsConstantBufferView(3, lightCbGpu);
            }
            drawSkinnedCallback();
        }
    }
}
