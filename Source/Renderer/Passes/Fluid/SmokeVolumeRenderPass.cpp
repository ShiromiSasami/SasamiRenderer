#define NOMINMAX
#include "Renderer/Passes/Fluid/SmokeVolumeRenderPass.h"

#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Foundation/Tools/DebugOutput.h"

#include "Renderer/Scene/LightSystem.h"
#include "Foundation/Math/MathUtil.h"

#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    // =========================================================================
    // Camera CB layout (PushCameraCB slots), mirrored byte-for-byte in
    // SmokeVolume_VS.hlsl / SmokeVolume_PS.hlsl:
    //
    //   mvp (u_invViewProj)   = inverse view-projection, for reconstructing the
    //                           world-space ray from the fullscreen triangle's UV
    //                           (same convention as VolumetricCloudRenderPass).
    //   world (u_viewProj)    = FORWARD view-projection. This slot carries the
    //                           camera's world matrix in FluidSurfaceRenderPass,
    //                           but this pass has no per-object world transform
    //                           to put there, so -- exactly as
    //                           VolumetricCloudRenderPass repurposes it for
    //                           packed scalars -- it is repurposed to carry the
    //                           forward VP. The pixel shader needs it to
    //                           re-project the ray/box near-hit point back to
    //                           NDC depth and write it via SV_Depth, which is
    //                           the only way a fullscreen triangle (a single
    //                           planar depth) can be tested per-pixel against
    //                           real scene depth for occlusion.
    //   extra0 (u_camPos)     = camera world position (xyz), matching
    //                           FluidSurfaceRenderPass's extra0 convention.
    //                           This is the true ray origin; deriving it from
    //                           the near-plane unprojection instead would offset
    //                           the origin by the near-clip distance, which
    //                           matters at the scale of this volume.
    //   extra1                = reserved, currently zero.
    // =========================================================================

    // =========================================================================
    // IRenderPass interface
    // =========================================================================

    void SmokeVolumeRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
        builder.RequireLightSystem();
    }

    void SmokeVolumeRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        // Read rather than bound as a depth target. The volume needs the scene's distance along
        // each ray to stop marching at whatever is inside the box -- a hardware depth test can only
        // reject the whole fragment at one depth, which for a fullscreen triangle is the box's near
        // face. That let smoke draw over geometry standing in the middle of the volume.
        builder.Read("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool SmokeVolumeRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_initialized || !m_enabled || !m_smokeSim || !m_smokeSim->IsInitialized()) {
            return true;
        }
        if (!context.IsSatisfied()) {
            DebugLog("SmokeVolumeRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        auto* enc = inputs.execution.commandEncoder;

        enc->SetGraphicsPipelineLayout(m_pipelineLayoutBindable);
        enc->SetGraphicsPipeline(m_pipelineBindable);

        // --- Viewport / scissor ---
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        // --- [0] Camera CB (b0) ---
        // See the Camera CB layout comment above the class for the slot meanings.
        const float* invVp = inputs.camera.invPv;
        const float* vp = inputs.camera.pv;
        if (invVp == nullptr || vp == nullptr) {
            DebugLog("SmokeVolumeRenderPass::Execute: camera view-projection is unavailable.\n");
            return false;
        }
        const float* cameraPosPtr = inputs.camera.pos;
        const float extra0[4] = {
            cameraPosPtr ? cameraPosPtr[0] : 0.0f,
            cameraPosPtr ? cameraPosPtr[1] : 0.0f,
            cameraPosPtr ? cameraPosPtr[2] : 0.0f,
            0.0f,
        };
        // The scene's directional light, in the same convention FluidSurfaceRenderPass uses:
        // extra1.xyz is the light-to-surface direction and .w its intensity, extra2.rgb its colour.
        // Self-shadowing and the phase function both need it -- without a light direction the
        // plume can only be flat grey.
        float lightDirection[3] = { 0.0f, -1.0f, 0.0f };
        float lightColor[3] = { 1.0f, 1.0f, 1.0f };
        float lightIntensity = 1.0f;
        if (inputs.lighting.lightSystem != nullptr) {
            const RenderDirectionalLight light = inputs.lighting.lightSystem->GetDirectionalLightSettings();
            Math::DirectionFromYawPitch(light.yaw, light.pitch, lightDirection);
            lightColor[0] = light.color[0];
            lightColor[1] = light.color[1];
            lightColor[2] = light.color[2];
            lightIntensity = light.intensity;
        }
        const float extra1[4] = { lightDirection[0], lightDirection[1], lightDirection[2], lightIntensity };
        const float extra2[4] = { lightColor[0], lightColor[1], lightColor[2], 0.0f };

        const RhiGpuAddress cameraCbGpu =
            inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                              invVp,
                                                              vp,
                                                              extra0,
                                                              extra1,
                                                              extra2);
        if (cameraCbGpu == 0) {
            DebugLog("SmokeVolumeRenderPass::Execute: PushCameraCB returned 0.\n");
            return false;
        }
        enc->SetGraphicsConstantBufferView(0, cameraCbGpu);

        // --- [1] Smoke grid-info CB (b1), borrowed directly from SmokeVolumeSim ---
        const RhiGpuAddress smokeCbGpu = static_cast<RhiGpuAddress>(m_smokeSim->GetUpdateCBGpuVA());
        if (smokeCbGpu == 0) return true;
        enc->SetGraphicsConstantBufferView(1, smokeCbGpu);

        // --- [2] Smoke state buffer inline SRV (t0) ---
        const RhiGpuAddress stateGpuVA = static_cast<RhiGpuAddress>(m_smokeSim->GetCurrentStateGpuVA());
        if (stateGpuVA == 0) return true;
        enc->SetGraphicsShaderResourceView(2, stateGpuVA);

        // t1 - scene depth, for clamping the march at geometry inside the volume.
        if (inputs.gbuffer.depthSrv.ptr != 0) {
            enc->SetGraphicsDescriptorTable(3, { inputs.gbuffer.depthSrv.ptr });
        }

        // --- Fullscreen triangle, no vertex/index buffers ---
        enc->Draw({ 3u, 1u, 0u, 0u });

        return true;
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool SmokeVolumeRenderPass::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        if (!CreatePipeline(device)) {
            DebugLog("SmokeVolumeRenderPass::Initialize: CreatePipeline failed.\n");
            return false;
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool SmokeVolumeRenderPass::CreatePipeline(IRHIDevice& device)
    {
        RhiBindingRangeDesc bindings[4]{};
        bindings[0].type = RhiBindingType::ConstantBuffer;
        bindings[0].visibility = RhiShaderStageFlags::All;
        bindings[0].baseRegister = 0; // b0 - camera CB
        bindings[0].inlineRootDescriptor = true;

        bindings[1].type = RhiBindingType::ConstantBuffer;
        bindings[1].visibility = RhiShaderStageFlags::All;
        bindings[1].baseRegister = 1; // b1 - smoke grid-info CB
        bindings[1].inlineRootDescriptor = true;

        bindings[2].type = RhiBindingType::ShaderResource;
        bindings[2].visibility = RhiShaderStageFlags::Pixel;
        bindings[2].baseRegister = 0; // t0 - smoke state buffer
        bindings[2].inlineRootDescriptor = true;

        // t1 - scene depth. A texture cannot be an inline root descriptor the way the buffer above
        // can, so this one comes through a descriptor table.
        bindings[3].type = RhiBindingType::ShaderResource;
        bindings[3].visibility = RhiShaderStageFlags::Pixel;
        bindings[3].baseRegister = 1;
        bindings[3].descriptorCount = 1;

        // s0 - point sampling for the depth fetch. Filtering a depth buffer averages across silhouette
        // edges and produces surfaces that exist nowhere, which here would show as smoke leaking a
        // pixel past every edge.
        RhiStaticSamplerDesc depthSampler{};
        depthSampler.visibility = RhiShaderStageFlags::Pixel;
        depthSampler.shaderRegister = 0;
        depthSampler.linearFilter = false;
        depthSampler.clamp = true;

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = static_cast<uint32_t>(std::size(bindings));
        layoutDesc.staticSamplers = &depthSampler;
        layoutDesc.staticSamplerCount = 1u;
        layoutDesc.allowInputAssembler = true;
        m_pipelineLayout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!m_pipelineLayout.IsValid()) {
            DebugLog("SmokeVolumeRenderPass: CreateRhiPipelineLayout failed.\n");
            return false;
        }

        std::vector<uint8_t> vsBytecode;
        std::vector<uint8_t> psBytecode;
        std::string vsTarget, psTarget;
        ShaderCompilationService::ResolveEffectiveVsPsTargets(device.GetDevice(), "6_6", vsTarget, psTarget);
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("SmokeVolumeRenderPass", L"Fluid/SmokeVolume_VS.hlsl", "VSMain", vsTarget.c_str(), vsBytecode)) {
            DebugLog("SmokeVolumeRenderPass: VS compilation failed.\n");
            return false;
        }
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("SmokeVolumeRenderPass", L"Fluid/SmokeVolume_PS.hlsl", "PSMain", psTarget.c_str(), psBytecode)) {
            DebugLog("SmokeVolumeRenderPass: PS compilation failed.\n");
            return false;
        }

        RhiShaderModuleDesc shaders[2]{};
        shaders[0].bytecode = vsBytecode.data();
        shaders[0].bytecodeSize = vsBytecode.size();
        shaders[0].entryPoint = "VSMain";
        shaders[0].stage = RhiShaderStageFlags::Vertex;
        shaders[1].bytecode = psBytecode.data();
        shaders[1].bytecodeSize = psBytecode.size();
        shaders[1].entryPoint = "PSMain";
        shaders[1].stage = RhiShaderStageFlags::Pixel;

        const RhiFormat colorFormat = RhiFormat::R16G16B16A16Float; // SceneColor is HDR
        RhiGraphicsPipelineDesc pipelineDesc{};
        pipelineDesc.layout = m_pipelineLayout;
        pipelineDesc.shaders = shaders;
        pipelineDesc.shaderCount = static_cast<uint32_t>(std::size(shaders));
        pipelineDesc.vertexBindingCount = 0;
        pipelineDesc.vertexAttributeCount = 0;
        pipelineDesc.topology = RhiPrimitiveTopology::TriangleList;
        // No vertex/index buffers, so winding is whatever the SV_VertexID bit-trick produces;
        // culling it on one side would drop the triangle entirely.
        pipelineDesc.raster.cullMode = RhiCullMode::None;
        // Depth is no longer bound as a target -- the shader clamps the march against the scene
        // depth it samples, which is strictly more capable than a single per-fragment test.
        pipelineDesc.depthStencil.depthTestEnabled = false;
        pipelineDesc.depthStencil.depthWriteEnabled = false;
        // Reverse-Z: near is 1.0, far is 0.0. RhiDepthStencilStateDesc already defaults to
        // GreaterEqual; overriding it to Less/LessEqual here is exactly the bug already fixed once
        // in FluidSurfaceRenderPass and once in ParticleRenderPass -- the volume would either
        // vanish (tested against the wrong half of the range) or z-fight geometry at the same
        // depth. Left unset deliberately so it keeps tracking the default.
        pipelineDesc.depthStencil.stencilEnabled = false;
        pipelineDesc.blend.alphaBlendEnabled = true;
        pipelineDesc.colorFormats = &colorFormat;
        pipelineDesc.colorFormatCount = 1;
        pipelineDesc.depthStencilFormat = RhiFormat::Unknown;

        m_pipeline = device.CreateRhiGraphicsPipeline(pipelineDesc);
        if (!m_pipeline.IsValid()) {
            DebugLog("SmokeVolumeRenderPass: CreateRhiGraphicsPipeline failed.\n");
            return false;
        }

        // The device returns create-handles carrying an internal map id; command-encoder binding
        // expects handles whose id is the native pointer. Resolve the bindable forms once here.
        m_pipelineLayoutBindable = device.GetBindablePipelineLayoutHandle(m_pipelineLayout);
        m_pipelineBindable       = device.GetBindablePipelineHandle(m_pipeline);
        if (!m_pipelineLayoutBindable.IsValid() || !m_pipelineBindable.IsValid()) {
            DebugLog("SmokeVolumeRenderPass: failed to resolve bindable pipeline handles.\n");
            return false;
        }

        return true;
    }

} // namespace SasamiEngine::Render
