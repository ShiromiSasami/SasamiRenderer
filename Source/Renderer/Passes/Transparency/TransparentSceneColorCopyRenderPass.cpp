#include "Renderer/Passes/Transparency/TransparentSceneColorCopyRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiEngine::Render
{
    void TransparentSceneColorCopyRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
    }

    void TransparentSceneColorCopyRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        // This pass is a CopyResource, not a draw, so both resources are declared as copy
        // endpoints and the graph transitions them to COPY_SOURCE / COPY_DEST itself.
        //
        // Read("SceneColor") used to be the declaration here, which left the graph believing
        // SceneColor was PIXEL_SHADER_RESOURCE while CopySceneColorForTransmission issued its own
        // barrier claiming a before-state of RENDER_TARGET. The debug layer reported that
        // mismatch on every launch. Patching the manual barrier's states to agree only moved the
        // error, because the pass then changed SceneColor's end state behind the graph's back and
        // later passes tripped over it -- the fix has to be that the graph owns the transition.
        //
        // Both resources are imported with transitionToFinalState = true (RendererFrameGraph.cpp),
        // so leaving them in a copy state here is safe: the graph restores PIXEL_SHADER_RESOURCE
        // at frame end, and any pass in between gets a correct transition from the tracker.
        builder.ReadAsCopySource("SceneColor");
        builder.WriteAsCopyDest("TransmissionSceneColor");
        builder.DependsOnPrevious();
    }

    bool TransparentSceneColorCopyRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentSceneColorCopyRenderPass::Execute: runtime context is invalid.\n");
            return false;
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
