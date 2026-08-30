#include "Renderer/Passes/Geometry/GBufferRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Passes/Geometry/SkinnedOpaqueDrawUtility.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    void GBufferRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawOpaqueItems();
    }

    void GBufferRenderPass::Setup(RenderGraphBuilder& builder) const
    {
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

    bool GBufferRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("GBufferRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();

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
                services.drawOpaqueItems);

        if (services.drawSkinnedOpaqueItems) {
            SkinnedOpaqueDrawUtility::ExecuteSkinnedOpaqueDraw(inputs.execution.commandEncoder,
                                            *inputs.execution.pipelineStateCache,
                                            inputs.execution.pipelineStateCache->GetSkinnedGBufferPipelineState(),
                                            *inputs.execution.srvHeap,
                                            services.drawSkinnedOpaqueItems);
        }
        return true;
    }

    void GBufferRenderPass::Execute(IRhiCommandEncoder* enc,
                                          RenderPipelineStateCache& pipelineStateCache,
                                          DescriptorHeap& srvHeap,
                                          const Viewport& viewport,
                                          const Rect& scissorRect,
                                          bool useTessellation,
                                          bool useTessellationWireframe,
                                          bool useTessellationDebugColors,
                                          bool useMeshletDebugView,
                                          const std::function<void()>& drawCallback) const
    {
        if (!enc) {
            return;
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);

        // Ignored deliberately: these debug visualizations only exist as single-target
        // (SceneColor) PSOs. This pass writes an MRT GBuffer set, so honoring them needs
        // dedicated GBuffer-output PSO variants that do not exist yet -- tracked in
        // TODO.md. The matching GUI checkboxes therefore have no effect.
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

        // G-Buffer fill only needs the material bindings the draw callback sets;
        // lighting inputs belong to the deferred Lighting pass.
        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));

        if (drawCallback) {
            drawCallback();
        }
    }
}
