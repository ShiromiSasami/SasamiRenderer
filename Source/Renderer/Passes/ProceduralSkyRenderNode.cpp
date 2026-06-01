#include "Renderer/Passes/ProceduralSkyRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    using Math::Mul4x4;

    void ProceduralSkyRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireSkybox();         // for cube VB
        builder.RequireLightSystem();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void ProceduralSkyRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool ProceduralSkyRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("ProceduralSkyRenderNode::Execute: runtime context is invalid.\n");
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

        const Skybox& skybox = *inputs.skybox;
        if (!skybox.IsSkyboxVBValid()) {
            DebugLog("ProceduralSkyRenderNode::Execute: skybox vertex buffer not ready.\n");
            return false;
        }

        auto*                     enc = inputs.execution.commandEncoder;
        RenderPipelineStateCache& pso = *inputs.execution.pipelineStateCache;

        // --- Root signature / PSO ---
        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pso.GetRootSignature()));
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pso.GetProceduralSkyPipelineState()));

        // --- Viewport / scissor ---
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        // --- Descriptor heap (root param 0 = SRV table; ProceduralSky PS samples no texture,
        //     but the root signature slot must be bound to a valid GPU handle) ---
        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(*inputs.execution.srvHeap));
        // Bind IBL SRV table as a dummy at slot 0 — ProceduralSky_PS.hlsl does not declare t0
        enc->SetGraphicsDescriptorTable(0, { skybox.GetIblSrvTable().ptr });

        // --- Camera CB ---
        const float* cameraPV  = inputs.camera.pv;
        const float* cameraPos = inputs.camera.pos;

        float skyboxWorld[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            cameraPos[0], cameraPos[1], cameraPos[2], 1,
        };
        float skyboxMVP[16];
        Mul4x4(skyboxWorld, cameraPV, skyboxMVP);

        const RenderDirectionalLight& light = inputs.lighting.lightSystem->GetDirectionalLightSettings();
        float lightForward[3] = {};
        Math::DirectionFromYawPitch(light.yaw, light.pitch, lightForward);

        const float sunDir[4] = {
            -lightForward[0],
            -lightForward[1],
            -lightForward[2],
            0.0f
        };
        const float sunColor[4] = {
            light.color[0],
            light.color[1],
            light.color[2],
            light.intensity
        };
        // extra2: x=time, y=cloudCover, z=cloudDensity, w=unused
        const float skyParams[4] = {
            inputs.sceneTimeSec,
            m_cloudCover,
            m_cloudDensity,
            0.0f
        };

        auto& frameCoord = *inputs.execution.frameCoordinator;
        const RhiGpuAddress cameraCbGpu =
            frameCoord.PushCameraCB(*inputs.execution.frame,
                                    skyboxMVP,
                                    skyboxWorld,
                                    sunDir,
                                    sunColor,
                                    skyParams);
        if (cameraCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(2, cameraCbGpu);
        }

        // --- Draw skybox cube ---
        const VertexBufferView vbv = skybox.GetSkyboxVBV();
        const RhiVertexBufferView rhiVbv{ vbv.BufferLocation, vbv.StrideInBytes, vbv.SizeInBytes };
        enc->SetVertexBuffers(0, 1, &rhiVbv);
        enc->Draw({36u, 1u, 0u, 0u});

        return true;
    }
}
