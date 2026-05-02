#include "Renderer/Passes/VolumetricCloudRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void VolumetricCloudRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireLightSystem();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void VolumetricCloudRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool VolumetricCloudRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!m_enabled) {
            return true;
        }
        if (!context.IsSatisfied()) {
            DebugLog("VolumetricCloudRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderNodeExecutionPolicy& policy = context.Policy();

        // Skip in non-final-lit modes (GBuffer debug views show raw buffers).
        if (policy.gBufferDebugView != RendererEnums::GBufferDebugView::FinalLit) {
            return true;
        }

        const RenderNodeFrameInputs& inputs = context.Inputs();

        if (!inputs.cameraInvPV) {
            DebugLog("VolumetricCloudRenderNode::Execute: cameraInvPV is null.\n");
            return false;
        }

        CommandList*              cmdList = inputs.cmdList;
        RenderPipelineStateCache& pso    = *inputs.pipelineStateCache;

        // --- Root signature / PSO ---
        cmdList->SetGraphicsRootSignature(pso.GetVolumetricCloudRootSignature());
        cmdList->SetPipelineState(pso.GetVolumetricCloudPipelineState());

        // --- Viewport / scissor ---
        cmdList->RSSetViewports(1, inputs.viewport);
        cmdList->RSSetScissorRects(1, inputs.scissorRect);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // --- Build VolumetricCloudCB packed into PushCameraCB slots ---
        // mvp  [16]     = invVP  (for world-space ray reconstruction)
        // world[0..3]   = camPos.xyz, sceneTime
        // world[4..7]   = sunDir.xyz (toward sun), sunIntensity
        // world[8..11]  = sunColor.rgb, cloudCover
        // world[12..15] = cloudDensity, windSpeed, cloudBaseAlt, cloudTopAlt
        // extra0[4]     = viewportWidth, viewportHeight, 0, 0
        // extra1[4]     = reserved
        // extra2[4]     = reserved

        const RenderDirectionalLight& light = inputs.lightSystem->GetDirectionalLightSettings();
        float lightFwd[3] = {};
        Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);

        const float world[16] = {
            inputs.cameraPos[0], inputs.cameraPos[1], inputs.cameraPos[2], inputs.sceneTimeSec,
            -lightFwd[0],         -lightFwd[1],         -lightFwd[2],        light.intensity,
            light.color[0],       light.color[1],        light.color[2],     m_cloudCover,
            m_cloudDensity,       m_windSpeed,            m_cloudBaseAlt,     m_cloudTopAlt,
        };
        const float extra0[4] = {
            inputs.viewport ? inputs.viewport->Width  : 1.0f,
            inputs.viewport ? inputs.viewport->Height : 1.0f,
            0.0f, 0.0f
        };
        const float extra1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        const D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
            inputs.frameCoordinator->PushCameraCB(*inputs.frame,
                                                  inputs.cameraInvPV,  // as mvp slot = invVP
                                                  world,
                                                  extra0,
                                                  extra1,
                                                  extra2);
        if (cbGpu != 0) {
            // Root param 0 = CBV b0 in the VolumetricCloud root signature.
            cmdList->SetGraphicsRootConstantBufferView(0, cbGpu);
        }

        // --- Fullscreen triangle (no VB/IB needed) ---
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);

        return true;
    }
}
