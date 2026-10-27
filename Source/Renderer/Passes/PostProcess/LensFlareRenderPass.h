#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiEngine::Render
{
    // Ghost-based lens flare -- Kawase (Silicon Studio), "物理ベース？アート指向？
    // ～尤もらしさと自由度を両立するレンズフレア表現～", CEDEC 2019.
    //
    // The ghosts are generated procedurally from a hash of the ghost index rather than authored,
    // which is the talk's practical claim: position and size can be random as long as the scatter
    // is power-biased instead of uniform. So this pass sends the light's screen position and a
    // count, not a sprite table.
    //
    // Runs in the "PostProcess" phase, after LightShaftRenderPass and before FxaaRenderPass, and
    // shares their shape: the back buffer cannot be sampled while bound as a render target, so
    // Execute() copies it first -- here to test whether the light is actually visible, not to
    // blur it. The pipeline state blends ONE/ONE; the shader outputs only the flare.
    // Which of the three approaches to lens flare this pass reproduces. They exist side by side so
    // they can be compared from the same viewpoint, which is the only honest way to see what each
    // one buys: the differences are structural, not a matter of tuning.
    enum class LensFlareMode
    {
        // Hand-authored sprite table. The industry default before anyone modelled the optics.
        // Free and art-directable, but the table is fixed, so it cannot answer to aperture or zoom.
        Sprite = 0,
        // Kawase (Silicon Studio), CEDEC 2019. Ghost placement and size come from biased random
        // numbers, the aperture from a procedural distance field -- plausible and still directable.
        SiliconStudio = 1,
        // Capcom RE:2023, "Ray Tracing Lens Flare". Rays are traced through the actual lens stack,
        // so ghost positions and shapes are derived rather than chosen. Not implemented yet.
        RayTraced = 2,
    };

    class LensFlareRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "LensFlare"; }
        std::string_view PhaseTag() const override { return "PostProcess"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        // Same shape as RenderTargetPool's SrvAllocFn: allocates `count` descriptors from
        // the renderer's shared shader-visible heap.
        using SrvAllocFn = std::function<bool(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)>;

        // (Re)creates the private copy texture when the size changes. Must be called once
        // per frame before the graph executes, since Execute() has no device access. The
        // descriptor is requested only on the first call -- the shared heap's allocator
        // bumps, so asking again per resize would leak a slot each time.
        bool EnsureResources(GraphicsDevice& device, const SrvAllocFn& allocateSrv, UINT width, UINT height);

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // Overall strength of the additive contribution.
        void SetIntensity(float intensity) { m_intensity = intensity; }
        float GetIntensity() const { return m_intensity; }

        void SetMode(LensFlareMode mode) { m_mode = mode; }
        LensFlareMode GetMode() const { return m_mode; }

        void SetBladeCount(int blades) { m_bladeCount = blades; }
        int GetBladeCount() const { return m_bladeCount; }

        // The ray-traced mode draws a traced grid instead of a fullscreen triangle, so it needs
        // geometry. Built once on first use; the resolution is fixed because a finer grid costs
        // vertices linearly while the ghost outline it resolves stops improving.
        static constexpr uint32_t kGridResolution = 17u;   // vertices per side
        // How many surface pairs (i, j) to draw. The full set grows as n^2 in the surface count and
        // most pairs land outside the sensor; the shader walks them in a fixed order so this simply
        // takes the first N.
        static constexpr uint32_t kGhostPathCount = 24u;
        bool EnsureGridGeometry(GraphicsDevice& device);

        // Current frame's back-buffer resource, copied from at the top of Execute(). Injected
        // per frame from Renderer::Render for the same reason FxaaRenderPass needs it:
        // RenderPassFrameInputs exposes no raw back-buffer resource.
        void SetBackBufferResource(ID3D12Resource* backBuffer) { m_backBufferResource = backBuffer; }

    private:
        Resource m_inputCopyTexture;
        // Lives in the renderer's main shader-visible heap, not a private one -- a private
        // heap would force a SetDescriptorHeaps inside Execute and invalidate the table
        // slots earlier passes left bound (see FxaaRenderPass for the debug-layer message).
        CpuDescriptorHandle m_inputCopySrvCpu{};
        GpuDescriptorHandle m_inputCopySrv{};
        bool m_srvAllocated = false;
        UINT m_width = 0;
        UINT m_height = 0;
        DXGI_FORMAT m_inputFormat = DXGI_FORMAT_UNKNOWN;
        bool m_enabled = false;   // off by default: it is an artistic effect, not a correctness fix
        float m_intensity = 2.5f;
        // Aperture blade count. Odd counts give the doubled star pattern real
        // diaphragms show; 7 is the common photographic default.
        int m_bladeCount = 7;
        LensFlareMode m_mode = LensFlareMode::SiliconStudio;
        Resource m_gridVertexBuffer;
        Resource m_gridIndexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_gridVbv{};
        D3D12_INDEX_BUFFER_VIEW m_gridIbv{};
        uint32_t m_gridIndexCount = 0u;
        ID3D12Resource* m_backBufferResource = nullptr;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
