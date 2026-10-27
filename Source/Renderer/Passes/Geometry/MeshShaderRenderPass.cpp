#define NOMINMAX
#include "Renderer/Passes/Geometry/MeshShaderRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiEngine::Render
{
    void MeshShaderRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireCommandEncoder();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void MeshShaderRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneDepth");
        builder.Write("GBufferAlbedo");
        builder.Write("GBufferNormal");
        builder.Write("GBufferMaterial");
        builder.Write("GBufferEmissive");
        builder.Write("GBufferSpecularWorkflow");
        builder.UseColorTarget("GBufferAlbedo");
        builder.UseColorTarget("GBufferNormal");
        builder.UseColorTarget("GBufferMaterial");
        builder.UseColorTarget("GBufferEmissive");
        builder.UseColorTarget("GBufferSpecularWorkflow");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool MeshShaderRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("MeshShaderRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();
        const RenderPassExecutionServices& services = context.Services();

        if (!policy.useMeshShader || !services.drawClusterItems) {
            return true;
        }

        services.drawClusterItems();
        return true;
    }
}
