#define NOMINMAX
#include "Renderer/Passes/Fluid/FluidSurfaceRenderPass.h"

#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Foundation/Tools/DebugOutput.h"

#include <string>
#include <vector>

namespace SasamiRenderer
{
    namespace
    {
        // Mesh topology resolution: fixed independently of the simulation's runtime
        // countX/countZ. FluidSurface_VS clamps sampled grid coordinates against the
        // sim's live countX/countZ, so a mismatch just means edge vertices sample a
        // clamped height rather than causing a crash or out-of-bounds access.
        constexpr uint32_t kMeshGridRes = 128u;

        struct GridVertex { float x; float z; };

        std::vector<GridVertex> GenerateGridVertices(uint32_t res)
        {
            std::vector<GridVertex> verts;
            verts.reserve(static_cast<size_t>(res) * res);
            for (uint32_t z = 0; z < res; ++z) {
                for (uint32_t x = 0; x < res; ++x) {
                    verts.push_back({ static_cast<float>(x), static_cast<float>(z) });
                }
            }
            return verts;
        }

        std::vector<uint32_t> GenerateGridIndices(uint32_t res)
        {
            std::vector<uint32_t> indices;
            const uint32_t quadsPerAxis = res - 1;
            indices.reserve(static_cast<size_t>(quadsPerAxis) * quadsPerAxis * 6);
            for (uint32_t z = 0; z < quadsPerAxis; ++z) {
                for (uint32_t x = 0; x < quadsPerAxis; ++x) {
                    const uint32_t i0 = z * res + x;
                    const uint32_t i1 = z * res + (x + 1);
                    const uint32_t i2 = (z + 1) * res + x;
                    const uint32_t i3 = (z + 1) * res + (x + 1);
                    indices.push_back(i0);
                    indices.push_back(i2);
                    indices.push_back(i1);
                    indices.push_back(i1);
                    indices.push_back(i2);
                    indices.push_back(i3);
                }
            }
            return indices;
        }

    } // anonymous namespace

    // =========================================================================
    // IRenderPass interface
    // =========================================================================

    void FluidSurfaceRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void FluidSurfaceRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool FluidSurfaceRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_initialized || !m_enabled || !m_fluidSim || !m_fluidSim->IsInitialized()) {
            return true;
        }
        if (!context.IsSatisfied()) {
            DebugLog("FluidSurfaceRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        if (m_gridIndexCount == 0) {
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

        // --- [0] Camera CB (b0) ---
        const float* vp = inputs.camera.pv;
        const float identity[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        const float* cameraPosPtr = inputs.camera.pos;
        const float extra0[4] = {
            cameraPosPtr ? cameraPosPtr[0] : 0.0f,
            cameraPosPtr ? cameraPosPtr[1] : 0.0f,
            cameraPosPtr ? cameraPosPtr[2] : 0.0f,
            0.0f,
        };
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
            DebugLog("FluidSurfaceRenderPass::Execute: PushCameraCB returned 0.\n");
            return false;
        }
        enc->SetGraphicsConstantBufferView(0, cameraCbGpu);

        // --- [1] Fluid grid-info CB (b1), borrowed directly from FluidHeightfieldSim ---
        const RhiGpuAddress fluidCbGpu = static_cast<RhiGpuAddress>(m_fluidSim->GetUpdateCBGpuVA());
        if (fluidCbGpu == 0) return true;
        enc->SetGraphicsConstantBufferView(1, fluidCbGpu);

        // --- [2] Height/velocity buffer inline SRV (t0) ---
        const RhiGpuAddress heightGpuVA = static_cast<RhiGpuAddress>(m_fluidSim->GetCurrentHeightGpuVA());
        if (heightGpuVA == 0) return true;
        enc->SetGraphicsShaderResourceView(2, heightGpuVA);

        // --- Vertex/index buffers + indexed draw ---
        enc->SetVertexBufferBindings(0, 1, &m_gridVbBinding);
        enc->SetIndexBufferBinding(m_gridIbBinding);
        enc->DrawIndexed({ m_gridIndexCount, 1u, 0u, 0, 0u });

        return true;
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool FluidSurfaceRenderPass::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        if (!CreatePipeline(device)) {
            DebugLog("FluidSurfaceRenderPass::Initialize: CreatePipeline failed.\n");
            return false;
        }
        if (!CreateGridMesh(device)) {
            DebugLog("FluidSurfaceRenderPass::Initialize: CreateGridMesh failed.\n");
            return false;
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool FluidSurfaceRenderPass::CreatePipeline(IRHIDevice& device)
    {
        RhiBindingRangeDesc bindings[3]{};
        bindings[0].type = RhiBindingType::ConstantBuffer;
        bindings[0].visibility = RhiShaderStageFlags::All;
        bindings[0].baseRegister = 0; // b0 - camera CB
        bindings[0].inlineRootDescriptor = true;

        bindings[1].type = RhiBindingType::ConstantBuffer;
        bindings[1].visibility = RhiShaderStageFlags::All;
        bindings[1].baseRegister = 1; // b1 - fluid grid-info CB
        bindings[1].inlineRootDescriptor = true;

        bindings[2].type = RhiBindingType::ShaderResource;
        bindings[2].visibility = RhiShaderStageFlags::Vertex;
        bindings[2].baseRegister = 0; // t0 - height/velocity buffer
        bindings[2].inlineRootDescriptor = true;

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = static_cast<uint32_t>(std::size(bindings));
        layoutDesc.allowInputAssembler = true;
        m_pipelineLayout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!m_pipelineLayout.IsValid()) {
            DebugLog("FluidSurfaceRenderPass: CreateRhiPipelineLayout failed.\n");
            return false;
        }

        std::vector<uint8_t> vsBytecode;
        std::vector<uint8_t> psBytecode;
        std::string vsTarget, psTarget;
        ShaderCompilationService::ResolveEffectiveVsPsTargets(device.GetDevice(), "6_6", vsTarget, psTarget);
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("FluidSurfaceRenderPass", L"Fluid/FluidSurface_VS.hlsl", "VSMain", vsTarget.c_str(), vsBytecode)) {
            DebugLog("FluidSurfaceRenderPass: VS compilation failed.\n");
            return false;
        }
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("FluidSurfaceRenderPass", L"Fluid/FluidSurface_PS.hlsl", "PSMain", psTarget.c_str(), psBytecode)) {
            DebugLog("FluidSurfaceRenderPass: PS compilation failed.\n");
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
        vertexBinding.stride = sizeof(GridVertex);
        vertexBinding.inputRate = RhiInputRate::PerVertex;

        RhiVertexAttributeDesc vertexAttributes[1]{};
        vertexAttributes[0].semantic = "POSITION";
        vertexAttributes[0].format = RhiFormat::R32G32Float;
        vertexAttributes[0].binding = 0;
        vertexAttributes[0].offset = 0;

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
        pipelineDesc.depthStencil.depthWriteEnabled = true;
        pipelineDesc.depthStencil.depthCompare = RhiCompareOp::Less;
        pipelineDesc.depthStencil.stencilEnabled = false;
        pipelineDesc.colorFormats = &colorFormat;
        pipelineDesc.colorFormatCount = 1;
        pipelineDesc.depthStencilFormat = RhiFormat::D32Float;

        m_pipeline = device.CreateRhiGraphicsPipeline(pipelineDesc);
        if (!m_pipeline.IsValid()) {
            DebugLog("FluidSurfaceRenderPass: CreateRhiGraphicsPipeline failed.\n");
            return false;
        }

        // The device returns create-handles carrying an internal map id; command-encoder binding
        // expects handles whose id is the native pointer. Resolve the bindable forms once here.
        m_pipelineLayoutBindable = device.GetBindablePipelineLayoutHandle(m_pipelineLayout);
        m_pipelineBindable       = device.GetBindablePipelineHandle(m_pipeline);
        if (!m_pipelineLayoutBindable.IsValid() || !m_pipelineBindable.IsValid()) {
            DebugLog("FluidSurfaceRenderPass: failed to resolve bindable pipeline handles.\n");
            return false;
        }

        return true;
    }

    // =========================================================================
    // CreateGridMesh
    // =========================================================================

    bool FluidSurfaceRenderPass::CreateGridMesh(IRHIDevice& device)
    {
        const std::vector<GridVertex> verts = GenerateGridVertices(kMeshGridRes);
        const std::vector<uint32_t>   indices = GenerateGridIndices(kMeshGridRes);
        if (verts.empty() || indices.empty()) return false;

        const UINT64 vbBytes = static_cast<UINT64>(verts.size() * sizeof(GridVertex));

        RhiBufferDesc vbDesc{};
        vbDesc.sizeInBytes = vbBytes;
        vbDesc.strideInBytes = sizeof(GridVertex);
        vbDesc.usage = RhiBufferUsageFlags::Vertex;
        vbDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
        vbDesc.initialState = RhiResourceState::Common;

        m_gridVB = device.CreateRhiBuffer(vbDesc, verts.data());
        if (!m_gridVB.IsValid()) {
            DebugLog("FluidSurfaceRenderPass: CreateRhiBuffer for grid VB failed.\n");
            return false;
        }

        m_gridVbBinding.buffer        = m_gridVB;
        m_gridVbBinding.offsetInBytes = 0;
        m_gridVbBinding.strideInBytes = sizeof(GridVertex);
        m_gridVbBinding.sizeInBytes   = static_cast<uint32_t>(vbBytes);

        const UINT64 ibBytes = static_cast<UINT64>(indices.size() * sizeof(uint32_t));

        RhiBufferDesc ibDesc{};
        ibDesc.sizeInBytes = ibBytes;
        ibDesc.strideInBytes = sizeof(uint32_t);
        ibDesc.usage = RhiBufferUsageFlags::Index;
        ibDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
        ibDesc.initialState = RhiResourceState::Common;

        m_gridIB = device.CreateRhiBuffer(ibDesc, indices.data());
        if (!m_gridIB.IsValid()) {
            DebugLog("FluidSurfaceRenderPass: CreateRhiBuffer for grid IB failed.\n");
            return false;
        }

        m_gridIbBinding.buffer        = m_gridIB;
        m_gridIbBinding.offsetInBytes = 0;
        m_gridIbBinding.sizeInBytes   = static_cast<uint32_t>(ibBytes);
        m_gridIbBinding.is32Bit       = true;

        m_gridIndexCount = static_cast<uint32_t>(indices.size());

        return true;
    }

} // namespace SasamiRenderer
