#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <cstdint>

namespace SasamiEngine::Render
{
    namespace Crowd { class CrowdSystem; }

    // Draws every crowd agent with one DrawIndexedInstanced into the GBuffer (TODO stage 8c).
    // No input layout: CrowdVat_VS fetches the animated vertex from the VAT tables by
    // SV_VertexID / SV_InstanceID (GPU Gems 3 ch.2 "Animated Crowd Rendering"; Unreal
    // AnimToTexture; SideFX VAT), places it with the agent's position + heading, and CrowdVat_PS
    // writes the five GBuffer targets with a constant material.
    // Root layout: b0 camera CB (PushCameraCB), t0 VAT table A, t1 VAT table B, t2 agents,
    // t3 clip table, t4 visible agent list (all inline root SRVs). Disabled by default; DX12 only.
    // TODO 8d: with culling on, the pass first lets CrowdSystem::RecordFrameWork record the GPU
    // step and CrowdGpuCuller, then issues one DrawIndexedIndirect per LOD, binding t4 at that
    // LOD's list (DX12 SV_InstanceID does not include StartInstanceLocation, so the offset goes
    // into the root SRV address instead). The VS switches to g_visible[instanceId] when
    // g_drawParams[0].x (the PushCameraCB world-matrix slot) holds asuint 1.
    class CrowdRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "Crowd"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        bool Initialize(IRHIDevice& device);
        // Pointer must outlive this pass (the renderer owns both).
        void SetCrowdSystem(Crowd::CrowdSystem* system) { m_system = system; }

        bool IsInitialized() const { return m_initialized; }
        bool IsEnabled()     const { return m_enabled; }
        void SetEnabled(bool e)    { m_enabled = e; }

        // Constant material written to the GBuffer (linear albedo, metallic-roughness workflow).
        void SetAlbedo(float r, float g, float b) { m_albedo[0] = r; m_albedo[1] = g; m_albedo[2] = b; }
        void SetRoughness(float r) { m_roughness = r; }

        // Shadow casting: called from the shadow pass's skinned-shadow callback with the shadow DSV,
        // viewport and the light view-projection already set. Draws every agent depth-only (no
        // culling: the camera-frustum list does not apply to the light). The shadow pass runs before
        // this pass, so it records the frame's pending GPU step itself; the crowd pass then finds
        // nothing pending and draws the same agent state.
        // The caller restores its own root signature / PSO afterwards.
        void DrawShadowDepth(IRhiCommandEncoder& enc,
                             RendererFrameCoordinator& frameCoordinator,
                             RendererFrameCoordinator::FrameContext& frame,
                             const float lightViewProjection[16],
                             bool d16Target) const;
        bool GetCastShadows() const { return m_castShadows; }
        void SetCastShadows(bool e) { m_castShadows = e; }

    private:
        bool CreatePipeline(IRHIDevice& device);

        IRHIDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_pipelineLayout{};
        RhiPipelineHandle       m_pipeline{};
        RhiPipelineLayoutHandle m_pipelineLayoutBindable{};
        RhiPipelineHandle       m_pipelineBindable{};
        // Depth-only shadow PSOs (CrowdVat_VS, no PS): D32 for the directional cascades,
        // D16 for spot / point shadow maps (same split as SkinnedShadow / SkinnedShadowD16).
        RhiPipelineHandle       m_shadowPipeline{};
        RhiPipelineHandle       m_shadowPipelineD16{};
        RhiPipelineHandle       m_shadowPipelineBindable{};
        RhiPipelineHandle       m_shadowPipelineD16Bindable{};

        // Non-const: Execute records the crowd's frame work (step + cull) through it.
        Crowd::CrowdSystem* m_system = nullptr;
        float m_albedo[3] = { 0.80f, 0.45f, 0.20f };
        float m_roughness = 0.7f;
        bool  m_initialized = false;
        bool  m_enabled = false;
        bool  m_castShadows = true;
    };
}

namespace SasamiEngine { using namespace Render; }
