#define NOMINMAX
#include "Renderer/Passes/Debug/DebugProbeGridRenderPass.h"

#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace SasamiRenderer
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        // UV sphere geometry.  Vertices are unit-radius (scale applied in VS via probeRadius).
        struct SphereVertex { float pos[3]; float normal[3]; };

        std::vector<SphereVertex> GenerateUVSphere(int rings, int segments)
        {
            std::vector<SphereVertex> verts;
            verts.reserve(static_cast<size_t>(segments * 3 * 2 + (rings - 2) * segments * 6));

            auto mkVert = [](float phi, float theta) -> SphereVertex {
                const float sp = std::sin(phi), cp = std::cos(phi);
                const float st = std::sin(theta), ct = std::cos(theta);
                SphereVertex sv{};
                sv.normal[0] = sp * ct; sv.normal[1] = cp; sv.normal[2] = sp * st;
                sv.pos[0] = sv.normal[0]; sv.pos[1] = sv.normal[1]; sv.pos[2] = sv.normal[2];
                return sv;
            };

            // Top cap
            const float phi1 = kPi / static_cast<float>(rings);
            for (int j = 0; j < segments; ++j) {
                const float t0 = (static_cast<float>(j)     / segments) * 2.0f * kPi;
                const float t1 = (static_cast<float>(j + 1) / segments) * 2.0f * kPi;
                verts.push_back(mkVert(0.0f, 0.0f));
                verts.push_back(mkVert(phi1, t0));
                verts.push_back(mkVert(phi1, t1));
            }

            // Middle bands
            for (int i = 1; i < rings - 1; ++i) {
                const float p0 = static_cast<float>(i)     / rings * kPi;
                const float p1 = static_cast<float>(i + 1) / rings * kPi;
                for (int j = 0; j < segments; ++j) {
                    const float t0 = (static_cast<float>(j)     / segments) * 2.0f * kPi;
                    const float t1 = (static_cast<float>(j + 1) / segments) * 2.0f * kPi;
                    // Upper-left triangle
                    verts.push_back(mkVert(p0, t0));
                    verts.push_back(mkVert(p1, t1));
                    verts.push_back(mkVert(p0, t1));
                    // Lower-right triangle
                    verts.push_back(mkVert(p0, t0));
                    verts.push_back(mkVert(p1, t0));
                    verts.push_back(mkVert(p1, t1));
                }
            }

            // Bottom cap
            const float botPhi = static_cast<float>(rings - 1) / rings * kPi;
            for (int j = 0; j < segments; ++j) {
                const float t0 = (static_cast<float>(j)     / segments) * 2.0f * kPi;
                const float t1 = (static_cast<float>(j + 1) / segments) * 2.0f * kPi;
                verts.push_back(mkVert(kPi, 0.0f));
                verts.push_back(mkVert(botPhi, t1));
                verts.push_back(mkVert(botPhi, t0));
            }

            return verts;
        }

    } // anonymous namespace

    // =========================================================================
    // IRenderPass interface
    // =========================================================================

    void DebugProbeGridRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void DebugProbeGridRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        // Depth-test against the scene so probes embed into opaque geometry while remaining unlit.
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool DebugProbeGridRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_enabled || !m_initialized || !m_probeGrid) {
            return true;
        }

        // Always flush the probe grid CB so VS/PS have the current origin/spacing/count
        // regardless of initialization order.  m_cbMapped is mutable so this is safe from
        // a const Execute().
        m_probeGrid->FlushGridCB();
        if (!context.IsSatisfied()) {
            DebugLog("DebugProbeGridRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        if (!m_probeGrid->IsInitialized()) {
            return true;
        }
        const uint32_t probeCount = m_probeGrid->GetTotalProbeCount();
        if (probeCount == 0 || m_sphereVertexCount == 0) {
            return true;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        auto* enc = inputs.execution.commandEncoder;

        enc->SetGraphicsPipelineLayout(m_pipelineLayoutBindable);
        enc->SetGraphicsPipeline(m_pipelineBindable);

        // --- Viewport / scissor ---
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        // --- [0] Camera CB (b0): viewProj + probeRadius in extra0.x ---
        const float* vp = inputs.camera.pv;
        const float identity[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        const float extra0[4] = { m_probeRadius, 0.0f, 0.0f, 0.0f };
        const float extra1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        const RhiGpuAddress cameraCbGpu =
            inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                            vp,
                                                   identity,
                                                   extra0,
                                                   extra1,
                                                   extra2);
        if (cameraCbGpu == 0) {
            DebugLog("DebugProbeGridRenderPass::Execute: PushCameraCB returned 0.\n");
            return false;
        }
        enc->SetGraphicsConstantBufferView(0, cameraCbGpu);

        // --- [1] GI Probe Grid CB (b2) ---
        const RhiGpuAddress probeCbGpu = m_probeGrid->GetProbeGridCbGpuAddress();
        if (probeCbGpu == 0) return true;
        enc->SetGraphicsConstantBufferView(1, probeCbGpu);

        // --- [2] Probe SH data inline SRV (t10) ---
        const RhiGpuAddress probeVA = m_probeGrid->GetProbeDataGpuVA();
        if (probeVA == 0) return true;
        enc->SetGraphicsShaderResourceView(2, probeVA);

        // --- Vertex buffer + instanced draw ---
        enc->SetVertexBufferBindings(0, 1, &m_sphereVbBinding);
        enc->Draw({ m_sphereVertexCount, probeCount, 0u, 0u });

        return true;
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool DebugProbeGridRenderPass::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        if (!CreatePipeline(device)) {
            DebugLog("DebugProbeGridRenderPass::Initialize: CreatePipeline failed.\n");
            return false;
        }
        if (!CreateSphereMesh(device)) {
            DebugLog("DebugProbeGridRenderPass::Initialize: CreateSphereMesh failed.\n");
            return false;
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool DebugProbeGridRenderPass::CreatePipeline(IRHIDevice& device)
    {
        RhiBindingRangeDesc bindings[3]{};
        bindings[0].type = RhiBindingType::ConstantBuffer;
        bindings[0].visibility = RhiShaderStageFlags::All;
        bindings[0].baseRegister = 0; // b0
        bindings[0].inlineRootDescriptor = true;

        bindings[1].type = RhiBindingType::ConstantBuffer;
        bindings[1].visibility = RhiShaderStageFlags::All;
        bindings[1].baseRegister = 2; // b2 (matches GI_Common.hlsli)
        bindings[1].inlineRootDescriptor = true;

        bindings[2].type = RhiBindingType::ShaderResource;
        bindings[2].visibility = RhiShaderStageFlags::Pixel;
        bindings[2].baseRegister = 10; // t10 (matches GI_Common.hlsli)
        bindings[2].inlineRootDescriptor = true;

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = static_cast<uint32_t>(std::size(bindings));
        layoutDesc.allowInputAssembler = true;
        m_pipelineLayout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!m_pipelineLayout.IsValid()) {
            DebugLog("DebugProbeGridRenderPass: CreateRhiPipelineLayout failed.\n");
            return false;
        }

        std::vector<uint8_t> vsBytecode;
        std::vector<uint8_t> psBytecode;
        std::string vsTarget, psTarget;
        ShaderCompilationService::ResolveEffectiveVsPsTargets(device.GetDevice(), "6_6", vsTarget, psTarget);
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("DebugProbeGrid", L"Debug/ProbeGrid/DebugProbeGrid_VS.hlsl", "VSMain", vsTarget.c_str(), vsBytecode)) {
            DebugLog("DebugProbeGridRenderPass: VS compilation failed.\n");
            return false;
        }
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("DebugProbeGrid", L"Debug/ProbeGrid/DebugProbeGrid_PS.hlsl", "PSMain", psTarget.c_str(), psBytecode)) {
            DebugLog("DebugProbeGridRenderPass: PS compilation failed.\n");
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

        RhiVertexBindingDesc vertexBinding{};
        vertexBinding.binding = 0;
        vertexBinding.stride = sizeof(SphereVertex);
        vertexBinding.inputRate = RhiInputRate::PerVertex;

        RhiVertexAttributeDesc vertexAttributes[2]{};
        vertexAttributes[0].semantic = "POSITION";
        vertexAttributes[0].format = RhiFormat::R32G32B32Float;
        vertexAttributes[0].binding = 0;
        vertexAttributes[0].offset = 0;
        vertexAttributes[1].semantic = "NORMAL";
        vertexAttributes[1].format = RhiFormat::R32G32B32Float;
        vertexAttributes[1].binding = 0;
        vertexAttributes[1].offset = 12;

        const RhiFormat colorFormat = RhiFormat::R16G16B16A16Float; // SceneColor is HDR
        RhiGraphicsPipelineDesc pipelineDesc{};
        pipelineDesc.layout = m_pipelineLayout;
        pipelineDesc.shaders = shaders;
        pipelineDesc.shaderCount = static_cast<uint32_t>(std::size(shaders));
        pipelineDesc.vertexBindings = &vertexBinding;
        pipelineDesc.vertexBindingCount = 1;
        pipelineDesc.vertexAttributes = vertexAttributes;
        pipelineDesc.vertexAttributeCount = static_cast<uint32_t>(std::size(vertexAttributes));
        pipelineDesc.topology = RhiPrimitiveTopology::TriangleList;
        pipelineDesc.raster.cullMode = RhiCullMode::Back;
        pipelineDesc.depthStencil.depthTestEnabled = true;
        pipelineDesc.depthStencil.depthWriteEnabled = false;
        pipelineDesc.depthStencil.depthCompare = RhiCompareOp::LessEqual;
        pipelineDesc.depthStencil.stencilEnabled = false;
        pipelineDesc.colorFormats = &colorFormat;
        pipelineDesc.colorFormatCount = 1;
        pipelineDesc.depthStencilFormat = RhiFormat::D32Float;

        m_pipeline = device.CreateRhiGraphicsPipeline(pipelineDesc);
        if (!m_pipeline.IsValid()) {
            DebugLog("DebugProbeGridRenderPass: CreateRhiGraphicsPipeline failed.\n");
            return false;
        }

        // The device returns create-handles carrying an internal map id; command-encoder binding
        // expects handles whose id is the native pointer. Resolve the bindable forms once here.
        m_pipelineLayoutBindable = device.GetBindablePipelineLayoutHandle(m_pipelineLayout);
        m_pipelineBindable       = device.GetBindablePipelineHandle(m_pipeline);
        if (!m_pipelineLayoutBindable.IsValid() || !m_pipelineBindable.IsValid()) {
            DebugLog("DebugProbeGridRenderPass: failed to resolve bindable pipeline handles.\n");
            return false;
        }

        return true;
    }

    // =========================================================================
    // CreateSphereMesh
    // =========================================================================

    bool DebugProbeGridRenderPass::CreateSphereMesh(IRHIDevice& device)
    {
        const std::vector<SphereVertex> verts = GenerateUVSphere(8, 12);
        if (verts.empty()) return false;

        const UINT64 vbBytes = static_cast<UINT64>(verts.size() * sizeof(SphereVertex));

        RhiBufferDesc vbDesc{};
        vbDesc.sizeInBytes = vbBytes;
        vbDesc.strideInBytes = sizeof(SphereVertex);
        vbDesc.usage = RhiBufferUsageFlags::Vertex;
        vbDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
        vbDesc.initialState = RhiResourceState::Common;

        m_sphereVB = device.CreateRhiBuffer(vbDesc, verts.data());
        if (!m_sphereVB.IsValid()) {
            DebugLog("DebugProbeGridRenderPass: CreateRhiBuffer for sphere VB failed.\n");
            return false;
        }

        m_sphereVbBinding.buffer        = m_sphereVB;
        m_sphereVbBinding.offsetInBytes = 0;
        m_sphereVbBinding.strideInBytes = sizeof(SphereVertex);
        m_sphereVbBinding.sizeInBytes   = static_cast<uint32_t>(vbBytes);
        m_sphereVertexCount             = static_cast<uint32_t>(verts.size());

        return true;
    }

} // namespace SasamiRenderer
