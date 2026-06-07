#include "Renderer/Passes/Sky/VolumetricCloudRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void VolumetricCloudRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireLightSystem();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void VolumetricCloudRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool VolumetricCloudRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_enabled) {
            return true;
        }
        if (!context.IsSatisfied()) {
            DebugLog("VolumetricCloudRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();

        // Skip in non-final-lit modes (GBuffer debug views show raw buffers).
        if (policy.gBufferDebugView != RendererEnums::GBufferDebugView::FinalLit) {
            return true;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();

        if (!inputs.camera.invPv) {
            DebugLog("VolumetricCloudRenderPass::Execute: cameraInvPV is null.\n");
            return false;
        }

        auto*                     enc = inputs.execution.commandEncoder;
        RenderPipelineStateCache& psc = *inputs.execution.pipelineStateCache;

        // --- Root signature / PSO ---
        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(psc.GetVolumetricCloudRootSignature()));
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(psc.GetVolumetricCloudPipelineState()));

        // --- Viewport / scissor ---
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        // --- Build VolumetricCloudCB packed into PushCameraCB slots ---
        // mvp  [16]     = invVP  (for world-space ray reconstruction)
        // world[0..3]   = camPos.xyz, sceneTime
        // world[4..7]   = sunDir.xyz (toward sun), sunIntensity
        // world[8..11]  = sunColor.rgb, cloudCover
        // world[12..15] = cloudDensity, windSpeed, cloudBaseAlt, cloudTopAlt
        // extra0[4]     = viewportWidth, viewportHeight, 0, 0
        // extra1[4]     = reserved
        // extra2[4]     = reserved

        const RenderDirectionalLight& light = inputs.lighting.lightSystem->GetDirectionalLightSettings();
        float lightFwd[3] = {};
        Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);

        const float world[16] = {
            inputs.camera.pos[0], inputs.camera.pos[1], inputs.camera.pos[2], inputs.sceneTimeSec,
            -lightFwd[0],          -lightFwd[1],          -lightFwd[2],         light.intensity,
            light.color[0],        light.color[1],         light.color[2],      m_cloudCover,
            m_cloudDensity,        m_windSpeed,             m_cloudBaseAlt,      m_cloudTopAlt,
        };
        const float extra0[4] = {
            inputs.execution.viewport ? inputs.execution.viewport->Width  : 1.0f,
            inputs.execution.viewport ? inputs.execution.viewport->Height : 1.0f,
            0.0f, 0.0f
        };
        const float extra1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        const RhiGpuAddress cbGpu =
            inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                            inputs.camera.invPv,  // as mvp slot = invVP
                                                            world,
                                                            extra0,
                                                            extra1,
                                                            extra2);
        if (cbGpu != 0) {
            // Root param 0 = CBV b0 in the VolumetricCloud root signature.
            enc->SetGraphicsConstantBufferView(0, cbGpu);
        }

        // --- Fullscreen triangle (no VB/IB needed) ---
        enc->Draw({3u, 1u, 0u, 0u});

        return true;
    }
}
