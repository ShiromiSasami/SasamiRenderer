#include "Renderer/Passes/RayMarchRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void RayMarchRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
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

        if (!inputs.cameraInvPV) {
            DebugLog("RayMarchRenderNode::Execute: cameraInvPV is null.\n");
            return false;
        }

        CommandList*              cmdList = inputs.cmdList;
        RenderPipelineStateCache& pso     = *inputs.pipelineStateCache;

        cmdList->SetGraphicsRootSignature(pso.GetRayMarchRootSignature());
        cmdList->SetPipelineState(pso.GetRayMarchPipelineState());

        cmdList->RSSetViewports(1, inputs.viewport);
        cmdList->RSSetScissorRects(1, inputs.scissorRect);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Build constant buffer (same layout as PushCameraCB 5-arg form)
        // mvp   -> cameraInvPV
        // world -> camPos/time/sunDir/sunIntensity/sunColor/renderSize
        // extra0    -> debugMode, tanHalfFovX, tanHalfFovY, rayMarchCameraEnabled
        // extra1    -> cameraRight.xyz
        // extra2    -> cameraUp.xyz
        const RenderDirectionalLight& light = inputs.lightSystem->GetDirectionalLightSettings();
        float lightFwd[3] = {};
        Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);

        const float world[16] = {
            inputs.cameraPos[0],  inputs.cameraPos[1],  inputs.cameraPos[2], inputs.sceneTimeSec,
            -lightFwd[0],         -lightFwd[1],          -lightFwd[2],        light.intensity,
            light.color[0],       light.color[1],         light.color[2],     m_cloudCover,
            inputs.viewport->Width, inputs.viewport->Height, 0.0f,            m_cloudDensity,
        };
        const float tanHalfFovY = inputs.cameraTanHalfFovY;
        const float tanHalfFovX = tanHalfFovY * inputs.cameraAspectRatio;
        const float rayMarchCameraEnabled =
            (inputs.cameraMode == RenderCameraMode::RayMarch) ? 1.0f : 0.0f;
        const float extra0[4] = { m_debugMode, tanHalfFovX, tanHalfFovY, rayMarchCameraEnabled };
        const float extra1[4] = {
            inputs.cameraRight ? inputs.cameraRight[0] : 1.0f,
            inputs.cameraRight ? inputs.cameraRight[1] : 0.0f,
            inputs.cameraRight ? inputs.cameraRight[2] : 0.0f,
            0.0f
        };
        const float extra2[4] = {
            inputs.cameraUp ? inputs.cameraUp[0] : 0.0f,
            inputs.cameraUp ? inputs.cameraUp[1] : 1.0f,
            inputs.cameraUp ? inputs.cameraUp[2] : 0.0f,
            0.0f
        };

        const D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
            inputs.frameCoordinator->PushCameraCB(*inputs.frame,
                                                  inputs.cameraInvPV,
                                                  world,
                                                  extra0,
                                                  extra1,
                                                  extra2);
        if (cbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(0, cbGpu);
        }

        // Fullscreen triangle — no VB, no index buffer
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);

        return true;
    }
}
