#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/GI/IrradianceProbeGrid.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <cstdint>

namespace SasamiRenderer
{
    // Renders each irradiance probe as a sphere coloured by its accumulated SH irradiance.
    // Each sphere fragment evaluates GI_EvaluateProbeSH for its surface normal, producing
    // the standard game-engine "probe grid debug view."
    //
    // Disabled by default.  Enable via Renderer::SetDebugProbeGridEnabled(true).
    class DebugProbeGridRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "DebugProbeGrid"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        // Called once from Renderer::Initialize after the pipeline state cache is ready.
        bool Initialize(IRHIDevice& device);

        // Must be called before Execute; pointer must remain valid for the lifetime of this node.
        void SetProbeGrid(const IrradianceProbeGrid* grid) { m_probeGrid = grid; }

        bool IsInitialized()  const { return m_initialized; }
        bool IsEnabled()      const { return m_enabled; }
        void SetEnabled(bool e)     { m_enabled = e; }

        float GetProbeRadius()      const { return m_probeRadius; }
        void  SetProbeRadius(float r)     { m_probeRadius = (r > 0.001f) ? r : 0.001f; }

    private:
        bool CreatePipeline(IRHIDevice& device);
        bool CreateSphereMesh(IRHIDevice& device);

        RhiPipelineLayoutHandle m_pipelineLayout{};
        RhiPipelineHandle       m_pipeline{};
        // Encoder-bindable forms (id = native pointer) resolved from the created handles above.
        RhiPipelineLayoutHandle m_pipelineLayoutBindable{};
        RhiPipelineHandle       m_pipelineBindable{};

        RhiBufferHandle        m_sphereVB{};
        RhiVertexBufferBinding m_sphereVbBinding{};
        uint32_t         m_sphereVertexCount = 0u;

        const IrradianceProbeGrid* m_probeGrid = nullptr;
        float    m_probeRadius  = 0.2f;   // World-space sphere radius
        bool     m_initialized  = false;
        bool     m_enabled      = false;  // Off by default
    };
}
