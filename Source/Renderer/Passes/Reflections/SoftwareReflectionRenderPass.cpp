#include "Renderer/Passes/Reflections/SoftwareReflectionRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    namespace
    {
        bool UsesReflectionDebugView(RendererEnums::GBufferDebugView view)
        {
            return view == RendererEnums::GBufferDebugView::ReflectionRadiance ||
                   view == RendererEnums::GBufferDebugView::ReflectionAlpha ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionHitDistance ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionComposite;
        }
    }

    void SoftwareReflectionRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
    }

    void SoftwareReflectionRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneColor");
        builder.Read("SceneDepth");
        builder.Write("SoftwareReflection");
        builder.DependsOnPrevious();
    }

    bool SoftwareReflectionRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("SoftwareReflectionRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();
        if (!policy.useSoftwareRayTracedReflections) {
            return true;
        }

        const bool shouldExecute =
            policy.gBufferDebugView == RendererEnums::GBufferDebugView::FinalLit ||
            UsesReflectionDebugView(policy.gBufferDebugView);
        if (!shouldExecute) {
            return true;
        }

        const RenderPassExecutionServices& services = context.Services();
        if (services.executeSoftwareReflections &&
            !services.executeSoftwareReflections()) {
            DebugLog("SoftwareReflectionRenderPass::Execute: software reflection pass failed.\n");
            return false;
        }
        return true;
    }
}
