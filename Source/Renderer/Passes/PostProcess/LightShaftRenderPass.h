#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiEngine::Render
{
    // God rays (crepuscular rays) as a screen-space radial blur of the tone-mapped image
    // toward the sun -- Mitchell, "Volumetric Light Scattering as a Post-Process",
    // GPU Gems 3 ch. 13 (2007).
    //
    // Runs in the "PostProcess" phase before FxaaRenderPass, and shares that pass's shape:
    // the back buffer cannot be sampled while it is bound as a render target, so Execute()
    // copies it into a private texture first. The difference is the blend -- the pipeline
    // state is ONE/ONE, so the shader contributes scattered light on top of the existing
    // image instead of replacing it.
    class LightShaftRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "LightShaft"; }
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

        // Overall strength of the additive contribution. The pass samples the tone-mapped
        // image rather than a dedicated occlusion mask, so bright surfaces bleed a little;
        // the default is deliberately well below 1.
        void SetIntensity(float intensity) { m_intensity = intensity; }
        float GetIntensity() const { return m_intensity; }

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
        float m_intensity = 0.6f;
        ID3D12Resource* m_backBufferResource = nullptr;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
