#include "Renderer/Passes/RayTracingRenderNode.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Core/RenderGraph.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

namespace SasamiRenderer
{
    void RayTracingRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireExecuteRayTracing();
    }

    void RayTracingRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneColor");
        builder.Write("SceneColor");
        builder.DependsOnPrevious();
    }

    bool RayTracingRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("RayTracingRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }

        const auto& services = context.Services();
        if (!services.executeRayTracing) {
            DebugLog("RayTracingRenderNode::Execute: executeRayTracing callback is missing.\n");
            return false;
        }
        return services.executeRayTracing();
    }
}
