#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Particles/ParticleSystem.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <cstdint>

namespace SasamiRenderer
{
    // Renders ParticleSystem's particle buffer as camera-facing alpha-blended
    // billboards. No vertex/index buffer: each billboard's 4 corners come from a
    // small static array in ParticleBillboard_VS, selected by SV_VertexID, with
    // per-particle position/size/color fetched via SV_InstanceID from the sim's
    // buffer (bound directly as an inline SRV, mirroring FluidSurfaceRenderPass's
    // direct GPU-VA binding of FluidHeightfieldSim's height buffer). Disabled by
    // default; enable via Renderer::SetParticlesEnabled(true).
    class ParticleRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "Particles"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        // Called once from Renderer::Initialize after the pipeline state cache is ready.
        bool Initialize(IRHIDevice& device);

        // Must be called before Execute; pointer must remain valid for the lifetime of this node.
        void SetParticleSystem(const ParticleSystem* sim) { m_particleSystem = sim; }

        bool IsInitialized() const { return m_initialized; }
        bool IsEnabled()     const { return m_enabled; }
        void SetEnabled(bool e)    { m_enabled = e; }

    private:
        bool CreatePipeline(IRHIDevice& device);

        RhiPipelineLayoutHandle m_pipelineLayout{};
        RhiPipelineHandle       m_pipeline{};
        // Encoder-bindable forms (id = native pointer) resolved from the created handles above.
        RhiPipelineLayoutHandle m_pipelineLayoutBindable{};
        RhiPipelineHandle       m_pipelineBindable{};

        const ParticleSystem* m_particleSystem = nullptr;
        bool m_initialized = false;
        bool m_enabled     = false; // Off by default
    };
}
