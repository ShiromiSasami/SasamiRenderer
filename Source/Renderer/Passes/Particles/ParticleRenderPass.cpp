#define NOMINMAX
#include "Renderer/Passes/Particles/ParticleRenderPass.h"

#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Foundation/Tools/DebugOutput.h"

#include <string>
#include <vector>

namespace SasamiRenderer
{
    // =========================================================================
    // IRenderPass interface
    // =========================================================================

    void ParticleRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void ParticleRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool ParticleRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_initialized || !m_enabled || !m_particleSystem || !m_particleSystem->IsInitialized()) {
            return true;
        }
        if (!context.IsSatisfied()) {
            DebugLog("ParticleRenderPass::Execute: requirements not satisfied.\n");
            return false;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        auto* enc = inputs.execution.commandEncoder;

        enc->SetGraphicsPipelineLayout(m_pipelineLayoutBindable);
        enc->SetGraphicsPipeline(m_pipelineBindable);

        // --- Viewport / scissor ---
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleStrip);

        // --- [0] Camera CB (b0) ---
        const float* vp = inputs.camera.pv;
        const float identity[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        const float extra0[4] = {
            inputs.camera.right ? inputs.camera.right[0] : 1.0f,
            inputs.camera.right ? inputs.camera.right[1] : 0.0f,
            inputs.camera.right ? inputs.camera.right[2] : 0.0f,
            0.0f
        };
        const float extra1[4] = {
            inputs.camera.up ? inputs.camera.up[0] : 0.0f,
            inputs.camera.up ? inputs.camera.up[1] : 1.0f,
            inputs.camera.up ? inputs.camera.up[2] : 0.0f,
            0.0f
        };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        const RhiGpuAddress cameraCbGpu =
            inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                              vp,
                                                              identity,
                                                              extra0,
                                                              extra1,
                                                              extra2);
        if (cameraCbGpu == 0) {
            DebugLog("ParticleRenderPass::Execute: PushCameraCB returned 0.\n");
            return false;
        }
        enc->SetGraphicsConstantBufferView(0, cameraCbGpu);

        // --- [1] Particle buffer inline SRV (t0) ---
        const RhiGpuAddress particleBufferGpu = static_cast<RhiGpuAddress>(m_particleSystem->GetParticleBufferGpuVA());
        if (particleBufferGpu == 0) return true;
        enc->SetGraphicsShaderResourceView(1, particleBufferGpu);

        const uint32_t capacity = m_particleSystem->GetCapacity();
        if (capacity == 0) return true;

        enc->Draw({ 4u, capacity, 0u, 0u });

        return true;
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool ParticleRenderPass::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        if (!CreatePipeline(device)) {
            DebugLog("ParticleRenderPass::Initialize: CreatePipeline failed.\n");
            return false;
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool ParticleRenderPass::CreatePipeline(IRHIDevice& device)
    {
        RhiBindingRangeDesc bindings[2]{};
        bindings[0].type = RhiBindingType::ConstantBuffer;
        bindings[0].visibility = RhiShaderStageFlags::All;
        bindings[0].baseRegister = 0; // b0 - camera CB
        bindings[0].inlineRootDescriptor = true;

        bindings[1].type = RhiBindingType::ShaderResource;
        bindings[1].visibility = RhiShaderStageFlags::Vertex;
        bindings[1].baseRegister = 0; // t0 - particle buffer
        bindings[1].inlineRootDescriptor = true;

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = static_cast<uint32_t>(std::size(bindings));
        layoutDesc.allowInputAssembler = true;
        m_pipelineLayout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!m_pipelineLayout.IsValid()) {
            DebugLog("ParticleRenderPass: CreateRhiPipelineLayout failed.\n");
            return false;
        }

        std::vector<uint8_t> vsBytecode;
        std::vector<uint8_t> psBytecode;
        std::string vsTarget, psTarget;
        ShaderCompilationService::ResolveEffectiveVsPsTargets(device.GetDevice(), "6_6", vsTarget, psTarget);
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("ParticleRenderPass", L"Particles/ParticleBillboard_VS.hlsl", "VSMain", vsTarget.c_str(), vsBytecode)) {
            DebugLog("ParticleRenderPass: VS compilation failed.\n");
            return false;
        }
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("ParticleRenderPass", L"Particles/ParticleBillboard_PS.hlsl", "PSMain", psTarget.c_str(), psBytecode)) {
            DebugLog("ParticleRenderPass: PS compilation failed.\n");
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
        pipelineDesc.topology = RhiPrimitiveTopology::TriangleStrip;
        pipelineDesc.raster.cullMode = RhiCullMode::None;
        pipelineDesc.depthStencil.depthTestEnabled = true;
        pipelineDesc.depthStencil.depthWriteEnabled = false;
        pipelineDesc.depthStencil.depthCompare = RhiCompareOp::Less;
        pipelineDesc.depthStencil.stencilEnabled = false;
        pipelineDesc.blend.alphaBlendEnabled = true;
        pipelineDesc.colorFormats = &colorFormat;
        pipelineDesc.colorFormatCount = 1;
        pipelineDesc.depthStencilFormat = RhiFormat::D32Float;

        m_pipeline = device.CreateRhiGraphicsPipeline(pipelineDesc);
        if (!m_pipeline.IsValid()) {
            DebugLog("ParticleRenderPass: CreateRhiGraphicsPipeline failed.\n");
            return false;
        }

        // The device returns create-handles carrying an internal map id; command-encoder binding
        // expects handles whose id is the native pointer. Resolve the bindable forms once here.
        m_pipelineLayoutBindable = device.GetBindablePipelineLayoutHandle(m_pipelineLayout);
        m_pipelineBindable       = device.GetBindablePipelineHandle(m_pipeline);
        if (!m_pipelineLayoutBindable.IsValid() || !m_pipelineBindable.IsValid()) {
            DebugLog("ParticleRenderPass: failed to resolve bindable pipeline handles.\n");
            return false;
        }

        return true;
    }

} // namespace SasamiRenderer
