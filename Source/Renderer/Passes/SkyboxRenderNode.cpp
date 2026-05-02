#include "Renderer/Passes/SkyboxRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void SkyboxRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireSkybox();
        builder.RequireLightSystem();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void SkyboxRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool SkyboxRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("SkyboxRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderNodeExecutionPolicy& policy = context.Policy();
        if (policy.renderPathMode == RendererEnums::RenderPathMode::SdfFluid) {
            return true;
        }
        if (policy.gBufferDebugView != RendererEnums::GBufferDebugView::FinalLit) {
            return true;
        }
        const RenderNodeFrameInputs& inputs = context.Inputs();

        Execute(inputs.cmdList,
                *inputs.skybox,
                *inputs.pipelineStateCache,
                *inputs.srvHeap,
                *inputs.viewport,
                *inputs.scissorRect,
                inputs.cameraPV,
                inputs.cameraPos,
                inputs.lightSystem->GetDirectionalLightSettings(),
                [inputs](const float mvp[16],
                         const float world[16],
                         const float extra0[4],
                         const float extra1[4],
                         const float extra2[4]) -> D3D12_GPU_VIRTUAL_ADDRESS {
                    return inputs.frameCoordinator->PushCameraCB(*inputs.frame,
                                                                 mvp,
                                                                 world,
                                                                 extra0,
                                                                 extra1,
                                                                 extra2);
                });
        return true;
    }

    void SkyboxRenderNode::Execute(CommandList* cmdList,
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
        skybox.Render(cmdList,
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
