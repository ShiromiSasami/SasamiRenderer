#include "Renderer/Passes/ShadowRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Scene/Skybox.h"

namespace SasamiRenderer
{
    void ShadowRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireCmdList();
        builder.RequirePipelineStateCache();
        builder.RequireSrvHeap();
        builder.RequireLightSystem();
        builder.RequireFrameLight();
        builder.RequireSkybox();
        builder.RequireCameraPos();
    }

    void ShadowRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("ShadowMap");
        builder.DependsOnPrevious();
    }

    bool ShadowRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("ShadowRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderNodeFrameInputs& inputs = context.Inputs();
        const RenderNodeExecutionPolicy& policy = context.Policy();
        const RenderNodeExecutionServices& services = context.Services();

        Execute(inputs.cmdList,
                *inputs.lightSystem,
                *inputs.frameLight,
                *inputs.pipelineStateCache,
                *inputs.srvHeap,
                inputs.cameraPos,
                policy.useShadowTessellationPath,
                inputs.skybox->IsIblEnabled(),
                policy.iblIntensity,
                inputs.skybox->GetIblPrefilterMaxMip(),
                inputs.skybox->HasDiffuseShCoefficients(),
                inputs.skybox->GetDiffuseShCoefficients(),
                policy.gBufferDebugView,
                services.drawShadowItems);
        return true;
    }

    void ShadowRenderNode::Execute(CommandList* cmdList,
                                   LightSystem& lightSystem,
                                   LightSystem::FrameResources& frameLight,
                                   RenderPipelineStateCache& pipelineStateCache,
                                   DescriptorHeap& srvHeap,
                                   const float cameraPos[3],
                                   bool useTessellationPath,
                                   bool iblEnabled,
                                   float iblIntensity,
                                   float iblPrefilterMaxMip,
                                   bool hasDiffuseSh,
                                   const float (*diffuseShCoefficients)[3],
                                   RendererEnums::GBufferDebugView debugView,
                                   const DrawCallback& drawCallback) const
    {
        lightSystem.ExecuteShadowPass(cmdList,
                                      frameLight,
                                      pipelineStateCache,
                                      srvHeap,
                                      cameraPos,
                                      useTessellationPath,
                                      iblEnabled,
                                      iblIntensity,
                                      iblPrefilterMaxMip,
                                      hasDiffuseSh,
                                      diffuseShCoefficients,
                                      debugView,
                                      drawCallback);
    }
}
