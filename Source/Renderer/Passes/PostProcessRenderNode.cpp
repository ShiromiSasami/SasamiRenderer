#include "Renderer/Passes/PostProcessRenderNode.h"
#include "Renderer/Core/RenderGraph.h"

namespace SasamiRenderer
{
    void PostProcessRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
    }

    void PostProcessRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneColor");
        builder.Write("SceneColor");
        builder.DependsOnPrevious();
    }

    bool PostProcessRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            return false;
        }
        const RenderNodeExecutionServices& services = context.Services();
        if (services.compositeSoftwareReflections &&
            !services.compositeSoftwareReflections()) {
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
