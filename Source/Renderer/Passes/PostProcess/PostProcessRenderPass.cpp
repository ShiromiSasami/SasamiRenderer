#include "Renderer/Passes/PostProcess/PostProcessRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    void PostProcessRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
    }

    void PostProcessRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneColor");
        builder.Write("BackBuffer");
        builder.UseColorTarget("BackBuffer");
        builder.DependsOnPrevious();
    }

    bool PostProcessRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            return false;
        }
        const RenderPassExecutionServices& services = context.Services();
        if (services.toneMapSceneColor &&
            !services.toneMapSceneColor()) {
            return false;
        }
        return true;
    }
}
