#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Fluid/SmokeVolumeSim.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <cstdint>

namespace SasamiEngine::Render
{
    // Raymarches the smoke density produced by SmokeVolumeSim and alpha-blends the result over
    // SceneColor. Draws a fullscreen triangle (no vertex/index buffers, mirroring the volumetric
    // cloud pass's shape) and intersects the camera ray with the volume's axis-aligned box
    // per-pixel in SmokeVolume_PS.hlsl. Disabled by default; enable via SetEnabled(true) once a
    // SmokeVolumeSim has been bound.
    class SmokeVolumeRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "SmokeVolume"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        // Called once from Renderer::Initialize after the pipeline state cache is ready.
        bool Initialize(IRHIDevice& device);

        // Must be called before Execute; pointer must remain valid for the lifetime of this node.
        void SetSmokeSim(const SmokeVolumeSim* sim) { m_smokeSim = sim; }

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

        const SmokeVolumeSim* m_smokeSim = nullptr;
        bool m_initialized = false;
        bool m_enabled     = false; // Off by default
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
