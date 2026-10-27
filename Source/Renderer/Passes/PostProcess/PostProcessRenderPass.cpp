#include "Renderer/Passes/PostProcess/PostProcessRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiEngine::Render
{
    void PostProcessRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireToneMapSceneColor();
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
        if (!services.toneMapSceneColor()) {
            return false;
        }
        return true;
    }
}
