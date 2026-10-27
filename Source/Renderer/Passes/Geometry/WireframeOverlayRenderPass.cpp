#include "Renderer/Passes/Geometry/WireframeOverlayRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiEngine::Render
{
    void WireframeOverlayRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawOpaqueItemsVertexPath();
    }

    void WireframeOverlayRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool WireframeOverlayRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_enabled) {
            return true;
        }
        if (!context.IsSatisfied()) {
            DebugLog("WireframeOverlayRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        const RenderPassExecutionServices& services = context.Services();

        Execute(inputs.execution.commandEncoder,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                *inputs.execution.scissorRect,
                services.drawOpaqueItemsVertexPath);
        return true;
    }

    void WireframeOverlayRenderPass::Execute(IRhiCommandEncoder* enc,
                                             RenderPipelineStateCache& pipelineStateCache,
                                             DescriptorHeap& srvHeap,
                                             const Viewport& viewport,
                                             const Rect& scissorRect,
                                             const std::function<void()>& drawCallback) const
    {
        if (!enc) {
            return;
        }
        PipelineState& pso = pipelineStateCache.GetWireframePipelineState();
        if (pso.Get() == nullptr) {
            return; // only the DX12 pipeline cache builds it
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pso));
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));

        if (drawCallback) {
            drawCallback();
        }
    }
}
