#include "Renderer/Passes/RayTracing/RayTracingRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiRenderer
{
    void RayTracingRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireExecuteRayTracing();
    }

    void RayTracingRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneColor");
        builder.Write("SceneColor");
        builder.DependsOnPrevious();
    }

    bool RayTracingRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("RayTracingRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const auto& services = context.Services();
        if (!services.executeRayTracing) {
            DebugLog("RayTracingRenderPass::Execute: executeRayTracing callback is missing.\n");
            return false;
        }
        return services.executeRayTracing();
    }
}
