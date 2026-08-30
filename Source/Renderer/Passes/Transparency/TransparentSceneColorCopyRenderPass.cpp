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
        // TransmissionSceneColor is imported into the graph (RendererFrameGraph.cpp), but it is
        // written here via CopySceneColorForTransmission's own CopyResource barriers (not
        // UseColorTarget), and those barriers already restore PIXEL_SHADER_RESOURCE afterward.
        // This Write() only records the dependency edge for TransparentCompositeRenderPass's
        // Read("TransmissionSceneColor"); the graph never needs to transition it here.
        builder.Write("TransmissionSceneColor");
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
