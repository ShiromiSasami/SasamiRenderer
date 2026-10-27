#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <cstdint>

namespace SasamiEngine::Render
{
    // Shading parameters of ParticleVolumeRenderPass (all in world units / linear radiance).
    struct ParticleVolumeStyle
    {
        float smokeExtinction = 0.3f;   // sigma_t per unit smoke density per metre
        float flameExtinction = 0.35f;  // sigma_t per unit flame density per metre (flames are thin)
        float flameEmission = 6.0f;     // emitted radiance scale of the blackbody colour
        float sootDarkening = 0.75f;    // albedo drop at soot 1 (smoke off the fire)
    };

    // Ray-marches a CPU-built particle density grid (the wind fire/smoke splat, ParticleSplatGrid)
    // as a participating medium and blends it over SceneColor. Each cell is a float4
    // (smokeDensity, soot 0..1, flameDensity, flameAge 0..1). Smoke scatters sunlight (Beer-Lambert
    // with a short self-shadow march and a Henyey-Greenstein phase, as SmokeVolume_PS); flames emit
    // blackbody light (Nguyen, Fedkiw & Jensen 2002) and absorb little. Each step is integrated
    // analytically, (S - S e^(-sigma_t d)) / sigma_t (Hillaire 2015, slide 28), so the result does
    // not depend on the step length the way a plain Riemann sum does.
    // The grid buffer is a D3D12 upload heap read as a root SRV: DX12-only for now (debt).
    class ParticleVolumeRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "ParticleVolume"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        bool Initialize(IRHIDevice& device);
        void Shutdown();

        // Copies the grid (dims product * 4 floats) into the next ring slot. Call once per frame at
        // most; an unchanged `version` is ignored. Returns false when the upload failed.
        bool SetVolumeData(const float origin[3], float cellSize, const uint32_t dims[3], const float* cells,
                           uint64_t version);
        void SetStyle(const ParticleVolumeStyle& style) { m_style = style; }
        const ParticleVolumeStyle& Style() const { return m_style; }

        bool IsInitialized() const { return m_initialized; }
        bool IsEnabled()     const { return m_enabled; }
        void SetEnabled(bool e)    { m_enabled = e; }

    private:
        static constexpr uint32_t RING_SIZE = 4;  // assumes fewer than 4 frames in flight (unverified)

        bool CreatePipeline(IRHIDevice& device);

        IRHIDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_pipelineLayout{};
        RhiPipelineHandle       m_pipeline{};
        RhiPipelineLayoutHandle m_pipelineLayoutBindable{};
        RhiPipelineHandle       m_pipelineBindable{};

        RhiBufferHandle m_ring[RING_SIZE]{};
        uint64_t m_ringCapacity = 0;    // bytes per slot
        uint32_t m_current = 0;         // slot Execute reads
        bool m_hasData = false;
        uint64_t m_version = ~0ull;
        float m_origin[3] = { 0.0f, 0.0f, 0.0f };
        float m_cellSize = 1.0f;
        uint32_t m_dims[3] = { 0, 0, 0 };
        ParticleVolumeStyle m_style;

        bool m_initialized = false;
        bool m_enabled = false;
    };
}

namespace SasamiEngine { using namespace Render; }
