#include "Renderer/Passes/PostProcessRenderNode.h"
#include "Renderer/Core/RenderGraph.h"

namespace SasamiRenderer
{
    void PostProcessRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
    }

    void PostProcessRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.Write("BackBuffer");
        builder.UseColorTarget("BackBuffer");
        builder.DependsOnPrevious();
    }

    bool PostProcessRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            return false;
        }
        const RenderNodeExecutionServices& services = context.Services();
        if (services.toneMapSceneColor &&
            !services.toneMapSceneColor()) {
            return false;
        }
        Execute({});
        return true;
    }

    void PostProcessRenderNode::Execute(const std::function<void()>& ensureSceneTargetsPrepared) const
    {
        if (ensureSceneTargetsPrepared) {
            ensureSceneTargetsPrepared();
        }
    }
}
