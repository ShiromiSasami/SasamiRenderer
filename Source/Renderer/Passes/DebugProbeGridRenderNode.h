#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/GI/IrradianceProbeGrid.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

#include <cstdint>

namespace SasamiRenderer
{
    // Renders each irradiance probe as a sphere coloured by its accumulated SH irradiance.
    // Each sphere fragment evaluates GI_EvaluateProbeSH for its surface normal, producing
    // the standard game-engine "probe grid debug view."
    //
    // Disabled by default.  Enable via Renderer::SetDebugProbeGridEnabled(true).
    class DebugProbeGridRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag()      const override { return "DebugProbeGrid"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

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

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

        Resource         m_sphereVB;
        VertexBufferView m_sphereVBV{};
        uint32_t         m_sphereVertexCount = 0u;

        const IrradianceProbeGrid* m_probeGrid = nullptr;
        float    m_probeRadius  = 0.2f;   // World-space sphere radius
        bool     m_initialized  = false;
        bool     m_enabled      = false;  // Off by default
    };
}
