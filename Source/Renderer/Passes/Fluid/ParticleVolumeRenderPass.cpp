#define NOMINMAX
#include "Renderer/Passes/Fluid/ParticleVolumeRenderPass.h"

#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"
#include "Foundation/Tools/DebugOutput.h"

#include "Renderer/Scene/LightSystem.h"
#include "Foundation/Math/MathUtil.h"

#include <algorithm>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    // Camera CB layout (PushCameraCB slots), mirrored in ParticleVolume_PS.hlsl:
    //   mvp    = inverse view-projection (ray reconstruction), world = forward view-projection
    //   extra0 = camera position, extra1 = light direction (light to surface) + intensity,
    //   extra2 = light colour, extra3 = grid origin + cell size, extra4 = dims (as float) + 0,
    //   extra5 = smoke sigma_t, flame sigma_t, flame emission, soot darkening.

    void ParticleVolumeRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
        builder.RequireLightSystem();
    }

    void ParticleVolumeRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        // The march stops at the scene depth it samples, as in SmokeVolumeRenderPass.
        builder.Read("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool ParticleVolumeRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_initialized || !m_enabled || !m_hasData) return true;
        if (!context.IsSatisfied()) {
            DebugLog("ParticleVolumeRenderPass::Execute: context is invalid.\n");
            return false;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        auto* enc = inputs.execution.commandEncoder;

        const RhiGpuAddress gridGpu = m_device->GetRhiBufferGpuAddress(m_ring[m_current]);
        if (gridGpu == 0) return true;  // not DX12

        const float* invVp = inputs.camera.invPv;
        const float* vp = inputs.camera.pv;
        if (invVp == nullptr || vp == nullptr) return false;

        const float* camPos = inputs.camera.pos;
        const float extra0[4] = { camPos ? camPos[0] : 0.0f, camPos ? camPos[1] : 0.0f, camPos ? camPos[2] : 0.0f, 1.0f };

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
        const float extra3[4] = { m_origin[0], m_origin[1], m_origin[2], m_cellSize };
        const float extra4[4] = { float(m_dims[0]), float(m_dims[1]), float(m_dims[2]), 0.0f };
        const float extra5[4] = { m_style.smokeExtinction, m_style.flameExtinction, m_style.flameEmission,
                                  m_style.sootDarkening };

        const RhiGpuAddress cameraCbGpu = inputs.execution.frameCoordinator->PushCameraCB(
            *inputs.execution.frame, invVp, vp, extra0, extra1, extra2, extra3, extra4, extra5);
        if (cameraCbGpu == 0) {
            DebugLog("ParticleVolumeRenderPass::Execute: PushCameraCB returned 0.\n");
            return false;
        }

        enc->SetGraphicsPipelineLayout(m_pipelineLayoutBindable);
        enc->SetGraphicsPipeline(m_pipelineBindable);
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        enc->SetGraphicsConstantBufferView(0, cameraCbGpu);
        enc->SetGraphicsShaderResourceView(1, gridGpu);
        if (inputs.gbuffer.depthSrv.ptr != 0) {
            enc->SetGraphicsDescriptorTable(2, { inputs.gbuffer.depthSrv.ptr });
        }
        enc->Draw({ 3u, 1u, 0u, 0u });
        return true;
    }

    bool ParticleVolumeRenderPass::SetVolumeData(const float origin[3], float cellSize, const uint32_t dims[3],
                                                 const float* cells, uint64_t version)
    {
        if (!m_initialized || cells == nullptr) return false;
        if (version == m_version && m_hasData) return true;

        const uint64_t cellCount = uint64_t(dims[0]) * dims[1] * dims[2];
        if (cellCount == 0) {
            m_hasData = false;
            m_version = version;
            return true;
        }
        const uint64_t bytes = cellCount * sizeof(float) * 4;
        if (bytes > m_ringCapacity) {
            // Slots still in flight may be read by the GPU; wait before releasing them.
            m_device->WaitForRhiIdle();
            for (RhiBufferHandle& slot : m_ring) {
                if (slot.IsValid()) m_device->DestroyRhiResource(slot);
                slot = {};
            }
            const uint64_t capacity = (std::max)(bytes + bytes / 2, uint64_t(64 * 1024));
            for (RhiBufferHandle& slot : m_ring) {
                slot = m_device->CreateRhiBuffer(
                    Voxel::GpuVoxelBuffers::MakeBufferDesc(capacity, sizeof(float) * 4,
                                   RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                                   RhiMemoryUsage::CpuToGpu, RhiResourceState::ShaderResource),
                    nullptr);
                if (!slot.IsValid()) {
                    DebugLog("ParticleVolumeRenderPass: grid buffer creation failed.\n");
                    m_ringCapacity = 0;
                    m_hasData = false;
                    return false;
                }
            }
            m_ringCapacity = capacity;
            m_current = 0;
        }

        const uint32_t next = (m_current + 1) % RING_SIZE;
        if (!m_device->UpdateRhiBuffer(m_ring[next], 0, cells, bytes)) {
            m_hasData = false;
            return false;
        }
        m_current = next;
        for (int i = 0; i < 3; ++i) {
            m_origin[i] = origin[i];
            m_dims[i] = dims[i];
        }
        m_cellSize = cellSize;
        m_version = version;
        m_hasData = true;
        return true;
    }

    bool ParticleVolumeRenderPass::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;
        m_device = &device;
        if (!CreatePipeline(device)) {
            DebugLog("ParticleVolumeRenderPass::Initialize: CreatePipeline failed.\n");
            return false;
        }
        m_initialized = true;
        return true;
    }

    void ParticleVolumeRenderPass::Shutdown()
    {
        if (m_device == nullptr) return;
        for (RhiBufferHandle& slot : m_ring) {
            if (slot.IsValid()) m_device->DestroyRhiResource(slot);
            slot = {};
        }
        m_ringCapacity = 0;
        m_hasData = false;
    }

    bool ParticleVolumeRenderPass::CreatePipeline(IRHIDevice& device)
    {
        RhiBindingRangeDesc bindings[3]{};
        bindings[0].type = RhiBindingType::ConstantBuffer;
        bindings[0].visibility = RhiShaderStageFlags::All;
        bindings[0].baseRegister = 0; // b0 - camera CB
        bindings[0].inlineRootDescriptor = true;

        bindings[1].type = RhiBindingType::ShaderResource;
        bindings[1].visibility = RhiShaderStageFlags::Pixel;
        bindings[1].baseRegister = 0; // t0 - float4 grid cells
        bindings[1].inlineRootDescriptor = true;

        bindings[2].type = RhiBindingType::ShaderResource;
        bindings[2].visibility = RhiShaderStageFlags::Pixel;
        bindings[2].baseRegister = 1; // t1 - scene depth (descriptor table)
        bindings[2].descriptorCount = 1;

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
            DebugLog("ParticleVolumeRenderPass: CreateRhiPipelineLayout failed.\n");
            return false;
        }

        std::vector<uint8_t> vsBytecode;
        std::vector<uint8_t> psBytecode;
        std::string vsTarget, psTarget;
        ShaderCompilationService::ResolveEffectiveVsPsTargets(device.GetDevice(), "6_6", vsTarget, psTarget);
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("ParticleVolumeRenderPass", L"Fluid/SmokeVolume_VS.hlsl", "VSMain", vsTarget.c_str(), vsBytecode)) {
            DebugLog("ParticleVolumeRenderPass: VS compilation failed.\n");
            return false;
        }
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("ParticleVolumeRenderPass", L"Fluid/ParticleVolume_PS.hlsl", "PSMain", psTarget.c_str(), psBytecode)) {
            DebugLog("ParticleVolumeRenderPass: PS compilation failed.\n");
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
        pipelineDesc.raster.cullMode = RhiCullMode::None;
        pipelineDesc.depthStencil.depthTestEnabled = false;
        pipelineDesc.depthStencil.depthWriteEnabled = false;
        pipelineDesc.depthStencil.stencilEnabled = false;
        // SRC_ALPHA / INV_SRC_ALPHA: the shader outputs un-premultiplied colour (accum / alpha).
        pipelineDesc.blend.alphaBlendEnabled = true;
        pipelineDesc.colorFormats = &colorFormat;
        pipelineDesc.colorFormatCount = 1;
        pipelineDesc.depthStencilFormat = RhiFormat::Unknown;

        m_pipeline = device.CreateRhiGraphicsPipeline(pipelineDesc);
        if (!m_pipeline.IsValid()) {
            DebugLog("ParticleVolumeRenderPass: CreateRhiGraphicsPipeline failed.\n");
            return false;
        }
        m_pipelineLayoutBindable = device.GetBindablePipelineLayoutHandle(m_pipelineLayout);
        m_pipelineBindable       = device.GetBindablePipelineHandle(m_pipeline);
        if (!m_pipelineLayoutBindable.IsValid() || !m_pipelineBindable.IsValid()) {
            DebugLog("ParticleVolumeRenderPass: failed to resolve bindable pipeline handles.\n");
            return false;
        }
        return true;
    }

} // namespace SasamiEngine::Render
