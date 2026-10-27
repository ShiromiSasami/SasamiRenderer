#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Fluid/FluidHeightfieldSim.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <cstdint>

namespace SasamiEngine::Render
{
    // Renders the water surface as an indexed grid mesh whose vertices are
    // displaced in the vertex shader by sampling FluidHeightfieldSim's current
    // height buffer. Disabled by default; enable via Renderer::SetFluidSurfaceEnabled(true).
    class FluidSurfaceRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "FluidSurface"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        // Called once from Renderer::Initialize after the pipeline state cache is ready.
        bool Initialize(IRHIDevice& device);

        // Must be called before Execute; pointer must remain valid for the lifetime of this node.
        void SetFluidSim(const FluidHeightfieldSim* sim) { m_fluidSim = sim; }

        // How far the surface normal bends the backdrop sample, in UV units at unit slope.
        // Water's IOR is 1.33, but a screen-space offset is not a real refraction integral --
        // this is the artistic knob that stands in for the depth the real bend would depend on.
        void SetRefractionStrength(float strength) { m_refractionStrength = strength; }
        float GetRefractionStrength() const { return m_refractionStrength; }

        bool IsInitialized() const { return m_initialized; }
        bool IsEnabled()     const { return m_enabled; }
        void SetEnabled(bool e)    { m_enabled = e; }

    private:
        bool CreatePipeline(IRHIDevice& device);
        bool CreateGridMesh(IRHIDevice& device);

        RhiPipelineLayoutHandle m_pipelineLayout{};
        RhiPipelineHandle       m_pipeline{};
        // Encoder-bindable forms (id = native pointer) resolved from the created handles above.
        RhiPipelineLayoutHandle m_pipelineLayoutBindable{};
        RhiPipelineHandle       m_pipelineBindable{};

        RhiBufferHandle        m_gridVB{};
        RhiVertexBufferBinding m_gridVbBinding{};
        RhiBufferHandle        m_gridIB{};
        RhiIndexBufferBinding  m_gridIbBinding{};
        uint32_t         m_gridIndexCount = 0u;

        const FluidHeightfieldSim* m_fluidSim = nullptr;
        float m_refractionStrength = 0.06f;
        bool m_initialized = false;
        bool m_enabled     = false; // Off by default
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
