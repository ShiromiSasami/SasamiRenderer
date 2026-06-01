#include "Renderer/Passes/RayMarchRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void RayMarchRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireLightSystem();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void RayMarchRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        // No DependsOnPrevious: this is the sole rendering pass in RayMarchApp.
    }

    bool RayMarchRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("RayMarchRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderNodeFrameInputs& inputs = context.Inputs();

        if (!inputs.camera.invPv) {
            DebugLog("RayMarchRenderNode::Execute: cameraInvPV is null.\n");
            return false;
        }

        auto*                     enc = inputs.execution.commandEncoder;
        RenderPipelineStateCache& psc = *inputs.execution.pipelineStateCache;

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(psc.GetRayMarchRootSignature()));
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(psc.GetRayMarchPipelineState()));

        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        // Build constant buffer (same layout as PushCameraCB 5-arg form)
        // mvp   -> cameraInvPV
        // world -> camPos/time/sunDir/sunIntensity/sunColor/renderSize
        // extra0    -> debugMode, tanHalfFovX, tanHalfFovY, rayMarchCameraEnabled
        // extra1    -> cameraRight.xyz
        // extra2    -> cameraUp.xyz
        const RenderDirectionalLight& light = inputs.lighting.lightSystem->GetDirectionalLightSettings();
        float lightFwd[3] = {};
        Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);

        const float world[16] = {
            inputs.camera.pos[0],  inputs.camera.pos[1],  inputs.camera.pos[2], inputs.sceneTimeSec,
            -lightFwd[0],          -lightFwd[1],           -lightFwd[2],         light.intensity,
            light.color[0],        light.color[1],          light.color[2],      m_cloudCover,
            inputs.execution.viewport->Width, inputs.execution.viewport->Height, 0.0f, m_cloudDensity,
        };
        const float tanHalfFovY = inputs.camera.tanHalfFovY;
        const float tanHalfFovX = tanHalfFovY * inputs.camera.aspectRatio;
        const float rayMarchCameraEnabled =
            (inputs.camera.mode == RenderCameraMode::RayMarch) ? 1.0f : 0.0f;
        const float extra0[4] = { m_debugMode, tanHalfFovX, tanHalfFovY, rayMarchCameraEnabled };
        const float extra1[4] = {
            inputs.camera.right ? inputs.camera.right[0] : 1.0f,
            inputs.camera.right ? inputs.camera.right[1] : 0.0f,
            inputs.camera.right ? inputs.camera.right[2] : 0.0f,
            0.0f
        };
        const float extra2[4] = {
            inputs.camera.up ? inputs.camera.up[0] : 0.0f,
            inputs.camera.up ? inputs.camera.up[1] : 1.0f,
            inputs.camera.up ? inputs.camera.up[2] : 0.0f,
            0.0f
        };

        const RhiGpuAddress cbGpu =
            inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                            inputs.camera.invPv,
                                                            world,
                                                            extra0,
                                                            extra1,
                                                            extra2);
        if (cbGpu != 0) {
            enc->SetGraphicsConstantBufferView(0, cbGpu);
        }

        // Fullscreen triangle — no VB, no index buffer
        enc->Draw({3u, 1u, 0u, 0u});

        return true;
    }
}
