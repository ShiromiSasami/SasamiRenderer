#include "Renderer/Passes/Fluid/FluidBackdropCopyRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiEngine::Render
{
    void FluidBackdropCopyRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
    }

    void FluidBackdropCopyRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        // Declared as copy endpoints so the graph owns the COPY_SOURCE / COPY_DEST transitions.
        // Issuing them by hand is what produced the barrier mismatch the transparency copy used to
        // report on every launch (see TransparentSceneColorCopyRenderPass for that history).
        builder.ReadAsCopySource("SceneColor");
        builder.WriteAsCopyDest("TransmissionSceneColor");
        builder.DependsOnPrevious();
    }

    bool FluidBackdropCopyRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("FluidBackdropCopyRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionServices& services = context.Services();
        if (services.copySceneColorForTransmission &&
            !services.copySceneColorForTransmission()) {
            DebugLog("FluidBackdropCopyRenderPass::Execute: scene color copy failed.\n");
            return false;
        }
        return true;
    }
}
