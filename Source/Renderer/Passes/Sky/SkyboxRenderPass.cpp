#include "Renderer/Passes/Sky/SkyboxRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void SkyboxRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireSkybox();
        builder.RequireLightSystem();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void SkyboxRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool SkyboxRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("SkyboxRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderPassExecutionPolicy& policy = context.Policy();
        if (policy.gBufferDebugView != RendererEnums::GBufferDebugView::FinalLit) {
            return true;
        }
        const RenderPassFrameInputs& inputs = context.Inputs();

        Execute(inputs.execution.commandEncoder,
                *inputs.skybox,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                *inputs.execution.scissorRect,
                inputs.camera.pv,
                inputs.camera.pos,
                inputs.lighting.lightSystem->GetDirectionalLightSettings(),
                [inputs](const float mvp[16],
                         const float world[16],
                         const float extra0[4],
                         const float extra1[4],
                         const float extra2[4]) -> D3D12_GPU_VIRTUAL_ADDRESS {
                    return inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                                           mvp,
                                                                           world,
                                                                           extra0,
                                                                           extra1,
                                                                           extra2);
                });
        return true;
    }

    void SkyboxRenderPass::Execute(IRhiCommandEncoder* enc,
                                   const Skybox& skybox,
                                   RenderPipelineStateCache& pipelineStateCache,
                                   DescriptorHeap& srvHeap,
                                   const Viewport& viewport,
                                   const Rect& scissorRect,
                                   const float cameraPV[16],
                                   const float cameraPos[3],
                                   const RenderDirectionalLight& directionalLight,
                                   const Skybox::PushCameraCbCallback& pushCameraCb) const
    {
        skybox.Render(enc,
                      pipelineStateCache,
                      srvHeap,
                      viewport,
                      scissorRect,
                      cameraPV,
                      cameraPos,
                      directionalLight,
                      pushCameraCb);
    }
}
