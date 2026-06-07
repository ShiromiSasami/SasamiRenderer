#include "Renderer/Passes/Reflections/SoftwareReflectionCompositeRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    void SoftwareReflectionCompositeRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
    }

    void SoftwareReflectionCompositeRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SoftwareReflection");
        builder.Write("SceneColor");
        builder.DependsOnPrevious();
    }

    bool SoftwareReflectionCompositeRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("SoftwareReflectionCompositeRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();
        if (!policy.useSoftwareRayTracedReflections ||
            (policy.gBufferDebugView != RendererEnums::GBufferDebugView::FinalLit &&
             policy.gBufferDebugView != RendererEnums::GBufferDebugView::SwrtReflectionComposite)) {
            return true;
        }

        const RenderPassExecutionServices& services = context.Services();
        if (services.compositeSoftwareReflections &&
            !services.compositeSoftwareReflections()) {
            DebugLog("SoftwareReflectionCompositeRenderPass::Execute: software reflection composite failed.\n");
            return false;
        }
        return true;
    }
}
