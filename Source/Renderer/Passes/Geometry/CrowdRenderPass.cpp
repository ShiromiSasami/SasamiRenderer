#define NOMINMAX
#include "Renderer/Passes/Geometry/CrowdRenderPass.h"

#include "Renderer/Crowd/CrowdGpuCuller.h"
#include "Renderer/Crowd/CrowdSystem.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Foundation/Tools/DebugOutput.h"

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    namespace
    {
        float UintBitsAsFloat(uint32_t value)
        {
            float f = 0.0f;
            std::memcpy(&f, &value, sizeof(f));
            return f;
        }
    }

    void CrowdRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireCameraPos();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void CrowdRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneDepth");
        builder.Write("GBufferAlbedo");
        builder.Write("GBufferNormal");
        builder.Write("GBufferMaterial");
        builder.Write("GBufferEmissive");
        builder.Write("GBufferSpecularWorkflow");
        builder.UseColorTarget("GBufferAlbedo");
        builder.UseColorTarget("GBufferNormal");
        builder.UseColorTarget("GBufferMaterial");
        builder.UseColorTarget("GBufferEmissive");
        builder.UseColorTarget("GBufferSpecularWorkflow");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool CrowdRenderPass::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        m_device = &device;
        const bool created = CreatePipeline(device);
        if (!created) {
            DebugLog("CrowdRenderPass::Initialize: CreatePipeline failed.\n");
        }
        m_initialized = created;
        return created;
    }

    bool CrowdRenderPass::CreatePipeline(IRHIDevice& device)
    {
        RhiBindingRangeDesc bindings[6]{};
        bindings[0].type = RhiBindingType::ConstantBuffer;
        bindings[0].visibility = RhiShaderStageFlags::All;
        bindings[0].baseRegister = 0; // b0
        bindings[0].inlineRootDescriptor = true;

        for (uint32_t i = 1; i < 6; ++i) {
            bindings[i].type = RhiBindingType::ShaderResource;
            bindings[i].visibility = RhiShaderStageFlags::Vertex;
            bindings[i].baseRegister = i - 1; // t0..t4 (t4 = per-LOD visible list)
            bindings[i].inlineRootDescriptor = true;
        }

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = static_cast<uint32_t>(std::size(bindings));
        layoutDesc.allowInputAssembler = false;
        m_pipelineLayout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!m_pipelineLayout.IsValid()) {
            DebugLog("CrowdRenderPass: CreateRhiPipelineLayout failed.\n");
            return false;
        }

        std::vector<uint8_t> vsBytecode;
        std::vector<uint8_t> psBytecode;
        std::string vsTarget, psTarget;
        ShaderCompilationService::ResolveEffectiveVsPsTargets(device.GetDevice(), "6_6", vsTarget, psTarget);
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("CrowdVat", L"Raster/Geometry/Crowd/CrowdVat_VS.hlsl", "VSMain", vsTarget.c_str(), vsBytecode)) {
            DebugLog("CrowdRenderPass: VS compilation failed.\n");
            return false;
        }
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("CrowdVat", L"Raster/Geometry/Crowd/CrowdVat_PS.hlsl", "PSMain", psTarget.c_str(), psBytecode)) {
            DebugLog("CrowdRenderPass: PS compilation failed.\n");
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

        const RhiFormat colorFormats[5] = {
            RhiFormat::R8G8B8A8UNormSrgb,
            RhiFormat::R16G16B16A16Float,
            RhiFormat::R8G8B8A8UNorm,
            RhiFormat::R16G16B16A16Float,
            RhiFormat::R8G8B8A8UNorm,
        };

        RhiGraphicsPipelineDesc pipelineDesc{};
        pipelineDesc.layout = m_pipelineLayout;
        pipelineDesc.shaders = shaders;
        pipelineDesc.shaderCount = static_cast<uint32_t>(std::size(shaders));
        pipelineDesc.topology = RhiPrimitiveTopology::TriangleList;
        pipelineDesc.raster.cullMode = RhiCullMode::None;
        pipelineDesc.depthStencil.depthTestEnabled = true;
        pipelineDesc.depthStencil.depthWriteEnabled = true;
        pipelineDesc.depthStencil.depthCompare = RhiCompareOp::GreaterEqual; // reverse-Z
        pipelineDesc.depthStencil.stencilEnabled = false;
        pipelineDesc.colorFormats = colorFormats;
        pipelineDesc.colorFormatCount = static_cast<uint32_t>(std::size(colorFormats));
        pipelineDesc.depthStencilFormat = RhiFormat::D32Float;

        m_pipeline = device.CreateRhiGraphicsPipeline(pipelineDesc);
        if (!m_pipeline.IsValid()) {
            DebugLog("CrowdRenderPass: CreateRhiGraphicsPipeline failed.\n");
            return false;
        }

        m_pipelineLayoutBindable = device.GetBindablePipelineLayoutHandle(m_pipelineLayout);
        m_pipelineBindable       = device.GetBindablePipelineHandle(m_pipeline);
        if (!m_pipelineLayoutBindable.IsValid() || !m_pipelineBindable.IsValid()) {
            DebugLog("CrowdRenderPass: failed to resolve bindable pipeline handles.\n");
            return false;
        }

        // Depth-only shadow variants: same VS, no PS, no colour targets. The rasterizer bias matches
        // the native shadow PSOs (ShadowRasterizerState.h for the D32 directional maps, the
        // point/spot D16 values in RenderPipelineStateCache.cpp); negative because of reverse-Z.
        RhiGraphicsPipelineDesc shadowDesc = pipelineDesc;
        shadowDesc.shaderCount = 1; // VS only
        shadowDesc.colorFormats = nullptr;
        shadowDesc.colorFormatCount = 0;
        shadowDesc.depthStencilFormat = RhiFormat::D32Float;
        shadowDesc.raster.depthBias = -1000;
        shadowDesc.raster.depthBiasClamp = 0.0f;
        shadowDesc.raster.slopeScaledDepthBias = -2.0f;
        m_shadowPipeline = device.CreateRhiGraphicsPipeline(shadowDesc);
        shadowDesc.depthStencilFormat = RhiFormat::D16UNorm;
        shadowDesc.raster.depthBias = -24;
        shadowDesc.raster.depthBiasClamp = 0.01f;
        shadowDesc.raster.slopeScaledDepthBias = -0.5f;
        m_shadowPipelineD16 = device.CreateRhiGraphicsPipeline(shadowDesc);
        if (m_shadowPipeline.IsValid()) m_shadowPipelineBindable = device.GetBindablePipelineHandle(m_shadowPipeline);
        if (m_shadowPipelineD16.IsValid()) m_shadowPipelineD16Bindable = device.GetBindablePipelineHandle(m_shadowPipelineD16);
        if (!m_shadowPipelineBindable.IsValid() || !m_shadowPipelineD16Bindable.IsValid()) {
            // Not fatal: the crowd still draws, it just casts no shadow.
            DebugLog("CrowdRenderPass: shadow pipelines unavailable; crowd casts no shadow.\n");
        }

        return true;
    }

    bool CrowdRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!m_enabled || !m_initialized || !m_system || !m_system->HasAsset()
            || m_system->AgentCount() == 0 || !m_system->AgentBuffer().IsValid()) {
            return true;
        }
        if (!context.IsSatisfied()) {
            DebugLog("CrowdRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RhiGpuAddress agentAddress = m_device->GetRhiBufferGpuAddress(m_system->AgentBuffer());
        if (agentAddress == 0) {
            return true;
        }
        const Crowd::CrowdVatGpuAsset& asset = m_system->Asset();
        if (asset.TableAAddress() == 0 || asset.TableBAddress() == 0 || asset.ClipTableAddress() == 0) {
            return true;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        auto* enc = inputs.execution.commandEncoder;

        enc->BeginDebugLabel("Crowd");

        bool culled = false;
        std::string err;
        if (!m_system->RecordFrameWork(*enc, inputs.camera.pv, inputs.camera.pos, culled, err)) {
            DebugLog(("CrowdRenderPass: RecordFrameWork failed: " + err + "\n").c_str());
            culled = false;
        }

        RhiResourceTransitionDesc toSrv{};
        toSrv.resource = m_system->AgentBuffer();
        toSrv.before = RhiResourceState::UnorderedAccess;
        toSrv.after = RhiResourceState::ShaderResource;
        enc->TransitionResources(&toSrv, 1);

        enc->SetGraphicsPipelineLayout(m_pipelineLayoutBindable);
        enc->SetGraphicsPipeline(m_pipelineBindable);

        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        float drawParams[16] = {};
        drawParams[0] = UintBitsAsFloat(culled ? 1u : 0u);
        const uint32_t modeBits = (asset.Mode() == Crowd::VatMode::Bone) ? 1u : 0u;
        const float extra0[4] = {
            m_system->ModelScale(),
            UintBitsAsFloat(asset.VertexCount()),
            UintBitsAsFloat(modeBits),
            UintBitsAsFloat(asset.BoneCount()),
        };
        const float* camPos = inputs.camera.pos;
        const float extra1[4] = { camPos[0], camPos[1], camPos[2], m_system->ModelYOffset() };
        const float extra2[4] = { m_albedo[0], m_albedo[1], m_albedo[2], m_roughness };

        const RhiGpuAddress cameraCbGpu =
            inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                            inputs.camera.pv,
                                                            drawParams,
                                                            extra0,
                                                            extra1,
                                                            extra2);
        if (cameraCbGpu == 0) {
            DebugLog("CrowdRenderPass::Execute: PushCameraCB returned 0.\n");
            RhiResourceTransitionDesc toUavOnError{};
            toUavOnError.resource = m_system->AgentBuffer();
            toUavOnError.before = RhiResourceState::ShaderResource;
            toUavOnError.after = RhiResourceState::UnorderedAccess;
            enc->TransitionResources(&toUavOnError, 1);
            m_system->EndFrameWork(*enc);
            enc->EndDebugLabel();
            return false;
        }

        enc->SetGraphicsConstantBufferView(0, cameraCbGpu);
        enc->SetGraphicsShaderResourceView(1, asset.TableAAddress());
        enc->SetGraphicsShaderResourceView(2, asset.TableBAddress());
        enc->SetGraphicsShaderResourceView(3, agentAddress);
        enc->SetGraphicsShaderResourceView(4, asset.ClipTableAddress());

        enc->SetIndexBufferBinding(asset.IndexBinding());
        if (culled) {
            const Crowd::CrowdGpuCuller& culler = m_system->Culler();
            for (uint32_t lod = 0; lod < culler.RecordedLodCount(); ++lod) {
                // DX12 SV_InstanceID excludes StartInstanceLocation, so the per-LOD list offset is baked into the root SRV address.
                enc->SetGraphicsShaderResourceView(5, culler.VisibleAddress() + static_cast<RhiGpuAddress>(lod) * culler.Capacity() * 4u);
                RhiDrawIndexedIndirectDesc d{};
                d.argsBuffer = culler.ArgsBuffer();
                d.argsOffsetInBytes = lod * Crowd::CrowdGpuCuller::kArgsStrideBytes;
                d.maxDrawCount = 1;
                enc->DrawIndexedIndirect(d);
            }
        } else {
            enc->SetGraphicsShaderResourceView(5, agentAddress); // unused by the VS when the cull flag is 0
            enc->DrawIndexed({ asset.IndexCount(), m_system->AgentCount(), 0u, 0, 0u });
        }

        RhiResourceTransitionDesc toUav{};
        toUav.resource = m_system->AgentBuffer();
        toUav.before = RhiResourceState::ShaderResource;
        toUav.after = RhiResourceState::UnorderedAccess;
        enc->TransitionResources(&toUav, 1);

        m_system->EndFrameWork(*enc);

        enc->EndDebugLabel();
        return true;
    }

    void CrowdRenderPass::DrawShadowDepth(IRhiCommandEncoder& enc,
                                          RendererFrameCoordinator& frameCoordinator,
                                          RendererFrameCoordinator::FrameContext& frame,
                                          const float lightViewProjection[16],
                                          bool d16Target) const
    {
        const RhiPipelineHandle pipeline = d16Target ? m_shadowPipelineD16Bindable : m_shadowPipelineBindable;
        if (!m_castShadows || !m_enabled || !m_initialized || !pipeline.IsValid() || !m_system
            || !m_system->HasAsset() || m_system->AgentCount() == 0 || !m_system->AgentBuffer().IsValid()) {
            return;
        }
        // Step first, so this frame's shadow matches this frame's draw. Recording a dispatch while
        // the shadow DSV is bound is legal on DX12, the only backend this pass runs on.
        std::string stepError;
        if (!m_system->RecordPendingStep(enc, stepError)) {
            DebugLog(("CrowdRenderPass: shadow-pass GPU step failed: " + stepError + "\n").c_str());
        }
        const RhiGpuAddress agentAddress = m_device->GetRhiBufferGpuAddress(m_system->AgentBuffer());
        const Crowd::CrowdVatGpuAsset& asset = m_system->Asset();
        if (agentAddress == 0 || asset.TableAAddress() == 0 || asset.TableBAddress() == 0 || asset.ClipTableAddress() == 0) {
            return;
        }

        float drawParams[16] = {}; // [0].x = 0: no g_visible indirection, every agent is drawn
        const uint32_t modeBits = (asset.Mode() == Crowd::VatMode::Bone) ? 1u : 0u;
        const float extra0[4] = {
            m_system->ModelScale(),
            UintBitsAsFloat(asset.VertexCount()),
            UintBitsAsFloat(modeBits),
            UintBitsAsFloat(asset.BoneCount()),
        };
        const float extra1[4] = { 0.0f, 0.0f, 0.0f, m_system->ModelYOffset() };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const RhiGpuAddress cbGpu = frameCoordinator.PushCameraCB(frame, lightViewProjection, drawParams,
                                                                  extra0, extra1, extra2);
        if (cbGpu == 0) {
            return;
        }

        // Between frames the agent buffer rests in UnorderedAccess (Execute / EndFrameWork leave it there).
        RhiResourceTransitionDesc toSrv{};
        toSrv.resource = m_system->AgentBuffer();
        toSrv.before = RhiResourceState::UnorderedAccess;
        toSrv.after = RhiResourceState::ShaderResource;
        enc.TransitionResources(&toSrv, 1);

        enc.SetGraphicsPipelineLayout(m_pipelineLayoutBindable);
        enc.SetGraphicsPipeline(pipeline);
        enc.SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        enc.SetGraphicsConstantBufferView(0, cbGpu);
        enc.SetGraphicsShaderResourceView(1, asset.TableAAddress());
        enc.SetGraphicsShaderResourceView(2, asset.TableBAddress());
        enc.SetGraphicsShaderResourceView(3, agentAddress);
        enc.SetGraphicsShaderResourceView(4, asset.ClipTableAddress());
        enc.SetGraphicsShaderResourceView(5, agentAddress); // unused when the cull flag is 0
        enc.SetIndexBufferBinding(asset.IndexBinding());
        enc.DrawIndexed({ asset.IndexCount(), m_system->AgentCount(), 0u, 0, 0u });

        RhiResourceTransitionDesc toUav{};
        toUav.resource = m_system->AgentBuffer();
        toUav.before = RhiResourceState::ShaderResource;
        toUav.after = RhiResourceState::UnorderedAccess;
        enc.TransitionResources(&toUav, 1);
    }
}
