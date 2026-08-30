#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Fluid/FluidHeightfieldSim.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <cstdint>

namespace SasamiRenderer
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
        bool m_initialized = false;
        bool m_enabled     = false; // Off by default
    };
}
