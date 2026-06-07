#include "Renderer/Passes/Transparency/TransparentSceneColorCopyRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiRenderer
{
    void TransparentSceneColorCopyRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
    }

    void TransparentSceneColorCopyRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneColor");
        builder.Write("TransmissionSceneColor");
        builder.DependsOnPrevious();
    }

    bool TransparentSceneColorCopyRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentSceneColorCopyRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();
        if (!policy.executeOpaqueFamilyPasses && !policy.executeLightingFamilyPasses) {
            return true;
        }

        const RenderPassExecutionServices& services = context.Services();
        if (services.copySceneColorForTransmission &&
            !services.copySceneColorForTransmission()) {
            DebugLog("TransparentSceneColorCopyRenderPass::Execute: transmission scene color copy failed.\n");
            return false;
        }
        return true;
    }
}
