#include "Renderer/Passes/SdfFluidRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void SdfFluidRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireLightSystem();
        builder.RequireCameraPV();   // cameraPV is unused but needed for IsSatisfied check
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void SdfFluidRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.DependsOnPrevious();
    }

    bool SdfFluidRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("SdfFluidRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderNodeExecutionPolicy& policy = context.Policy();

        // Only active in SdfFluid render mode
        if (policy.renderPathMode != RendererEnums::RenderPathMode::SdfFluid) {
            return true;
        }

        const RenderNodeFrameInputs& inputs = context.Inputs();

        // Require cameraInvPV (not validated by IsSatisfied since it's not a standard requirement)
        if (!inputs.cameraInvPV) {
            DebugLog("SdfFluidRenderNode::Execute: cameraInvPV is null.\n");
            return false;
        }

        CommandList*             cmdList  = inputs.cmdList;
        RenderPipelineStateCache& pso     = *inputs.pipelineStateCache;

        // --- Root signature / PSO ---
        cmdList->SetGraphicsRootSignature(pso.GetSdfFluidRootSignature());
        cmdList->SetPipelineState(pso.GetSdfFluidPipelineState());

        // --- Viewport / scissor ---
        cmdList->RSSetViewports(1, inputs.viewport);
        cmdList->RSSetScissorRects(1, inputs.scissorRect);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // --- Build SdfFluidCB packed into CameraCBData slots ---
        // mvp  [16]    -> cameraInvPV (invVP for ray reconstruction)
        // world[0..3]  -> camPos.xyz, time
        // world[4..7]  -> sunDir.xyz, sunIntensity
        // world[8..11] -> sunColor.rgb, cloudCover
        // world[12..15]-> renderW, renderH, fluidMode(float), cloudDensity
        // extra0[4]    -> fluidCenter.xyz, fluidRadius
        // extra1[4]    -> fluidColor.rgb, fluidDensity
        // extra2[4]    -> fluidSpeed, fluidDetail, fluidRoughness, fluidIOR

        const RenderDirectionalLight& light = inputs.lightSystem->GetDirectionalLightSettings();
        float lightFwd[3] = {};
        Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);

        const float world[16] = {
            inputs.cameraPos[0], inputs.cameraPos[1], inputs.cameraPos[2], inputs.sceneTimeSec,
            -lightFwd[0],        -lightFwd[1],         -lightFwd[2],       light.intensity,
            light.color[0],      light.color[1],        light.color[2],    m_cloudCover,
            inputs.viewport->Width, inputs.viewport->Height, static_cast<float>(m_fluidMode), m_cloudDensity,
        };
        const float extra0[4] = { m_center[0],  m_center[1],  m_center[2],  m_radius   };
        const float extra1[4] = { m_color[0],   m_color[1],   m_color[2],   m_density  };
        const float extra2[4] = { m_speed,       m_detail,     m_roughness,  m_ior      };

        const D3D12_GPU_VIRTUAL_ADDRESS cbGpu =
            inputs.frameCoordinator->PushCameraCB(*inputs.frame,
                                                  inputs.cameraInvPV, // as mvp slot = invVP
                                                  world,
                                                  extra0,
                                                  extra1,
                                                  extra2);
        if (cbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(0, cbGpu);
        }

        // --- Fullscreen triangle (no VB, no index buffer) ---
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);

        return true;
    }
}
