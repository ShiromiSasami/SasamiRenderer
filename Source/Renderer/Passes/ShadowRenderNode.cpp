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
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireViewport();
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

        if (policy.renderPathMode == RendererEnums::RenderPathMode::SdfFluid) {
            return true;
        }

        Execute(inputs.cmdList,
                *inputs.lightSystem,
                *inputs.frameLight,
                *inputs.pipelineStateCache,
                *inputs.srvHeap,
                inputs.cameraPos,
                inputs.cameraPV,
                policy.useSoftwareRayTracedDirectionalShadow,
                policy.useSoftwareRayTracedReflections,
                policy.reflectionMode,
                policy.useShadowTessellationPath,
                inputs.skybox->IsIblEnabled(),
                policy.iblIntensity,
                inputs.skybox->GetIblPrefilterMaxMip(),
                inputs.skybox->HasDiffuseShCoefficients(),
                inputs.skybox->GetDiffuseShCoefficients(),
                policy.gBufferDebugView,
                policy.softwareRayTracedShadowMapSize,
                policy.renderWidth,
                policy.renderHeight,
                services.executeSoftwareDirectionalShadow,
                services.executeSoftwareReflections,
                services.drawShadowItems);
        return true;
    }

    void ShadowRenderNode::Execute(CommandList* cmdList,
                                   LightSystem& lightSystem,
                                   LightSystem::FrameResources& frameLight,
                                   RenderPipelineStateCache& pipelineStateCache,
                                   DescriptorHeap& srvHeap,
                                   const float cameraPos[3],
                                   const float cameraPV[16],
                                   bool useSoftwareRayTracedDirectionalShadow,
                                   bool useSoftwareRayTracedReflections,
                                   float reflectionMode,
                                   bool useTessellationPath,
                                   bool iblEnabled,
                                   float iblIntensity,
                                   float iblPrefilterMaxMip,
                                   bool hasDiffuseSh,
                                   const float (*diffuseShCoefficients)[3],
                                   RendererEnums::GBufferDebugView debugView,
                                   uint32_t softwareRayTracedShadowMapSize,
                                   uint32_t renderWidth,
                                   uint32_t renderHeight,
                                   const std::function<bool(const LightSystem::ShadowPassContext&)>& softwareShadowCallback,
                                   const std::function<bool()>& softwareReflectionCallback,
                                   const DrawCallback& drawCallback) const
    {
        if (useSoftwareRayTracedDirectionalShadow) {
            lightSystem.UpdateFrameLighting(frameLight,
                                            cameraPos,
                                            cameraPV,
                                            iblEnabled,
                                            iblIntensity,
                                            iblPrefilterMaxMip,
                                            hasDiffuseSh,
                                            diffuseShCoefficients,
                                            debugView,
                                            softwareRayTracedShadowMapSize,
                                            softwareRayTracedShadowMapSize,
                                            reflectionMode,
                                            1.0f,
                                            renderWidth,
                                            renderHeight,
                                            true);

            if (!softwareShadowCallback) {
                DebugLog("ShadowRenderNode::Execute: software shadow callback is missing.\n");
                return;
            }

            LightSystem::ShadowPassContext shadowContext{};
            lightSystem.BuildStableDirectionalShadowPassContext(shadowContext,
                                                                softwareRayTracedShadowMapSize,
                                                                softwareRayTracedShadowMapSize);
            if (!softwareShadowCallback(shadowContext)) {
                DebugLog("ShadowRenderNode::Execute: software shadow callback failed.\n");
            }
            return;
        }

        lightSystem.ExecuteShadowPass(cmdList,
                                      frameLight,
                                      pipelineStateCache,
                                      srvHeap,
                                      cameraPos,
                                      cameraPV,
                                      useTessellationPath,
                                      iblEnabled,
                                      iblIntensity,
                                      iblPrefilterMaxMip,
                                      hasDiffuseSh,
                                      diffuseShCoefficients,
                                      debugView,
                                      softwareRayTracedShadowMapSize,
                                      softwareRayTracedShadowMapSize,
                                      reflectionMode,
                                      1.0f,
                                      renderWidth,
                                      renderHeight,
                                      drawCallback);
    }
}
