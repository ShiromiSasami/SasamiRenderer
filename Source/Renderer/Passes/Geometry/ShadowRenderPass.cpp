#include "Renderer/Passes/Geometry/ShadowRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Scene/Skybox.h"

namespace SasamiRenderer
{
    void ShadowRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
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

    void ShadowRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("ShadowMap");
        builder.DependsOnPrevious();
    }

    bool ShadowRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("ShadowRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderPassFrameInputs& inputs = context.Inputs();
        const RenderPassExecutionPolicy& policy = context.Policy();
        const RenderPassExecutionServices& services = context.Services();

        Execute(inputs.execution.cmdList,
                *inputs.lighting.lightSystem,
                *inputs.lighting.frameLight,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                inputs.camera.pos,
                inputs.camera.pv,
                policy.useSoftwareRayTracedDirectionalShadow,
                policy.reflectionMode,
                policy.useShadowTessellationPath,
                policy.vsmBlurEnabled,
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
                services.drawShadowItems);
        return true;
    }

    void ShadowRenderPass::Execute(CommandList* cmdList,
                                   LightSystem& lightSystem,
                                   LightSystem::FrameResources& frameLight,
                                   RenderPipelineStateCache& pipelineStateCache,
                                   DescriptorHeap& srvHeap,
                                   const float cameraPos[3],
                                   const float cameraPV[16],
                                   bool useSoftwareRayTracedDirectionalShadow,
                                   float reflectionMode,
                                   bool useTessellationPath,
                                   bool vsmBlurEnabled,
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
                DebugLog("ShadowRenderPass::Execute: software shadow callback is missing.\n");
                return;
            }

            LightSystem::ShadowPassContext shadowContext{};
            lightSystem.BuildStableDirectionalShadowPassContext(shadowContext,
                                                                softwareRayTracedShadowMapSize,
                                                                softwareRayTracedShadowMapSize);
            if (!softwareShadowCallback(shadowContext)) {
                DebugLog("ShadowRenderPass::Execute: software shadow callback failed.\n");
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
                                      drawCallback,
                                      vsmBlurEnabled);
    }
}
