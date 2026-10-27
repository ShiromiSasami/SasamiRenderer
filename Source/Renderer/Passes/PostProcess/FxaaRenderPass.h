#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiEngine::Render
{
    // Fullscreen FXAA anti-aliasing pass, placed after tone mapping ("PostProcess" tag) in
    // the frame graph. Reads a private copy of the tone-mapped back buffer and writes the
    // antialiased result back into "BackBuffer".
    //
    // RenderPassFrameInputs (RenderPassSetupContext.h) has no field exposing the raw
    // back-buffer resource, and that header is outside this pass's ownership, so the
    // back-buffer pointer is instead injected once per frame via SetBackBufferResource()
    // from Renderer::Render (RendererFrameGraph.cpp) -- the same "push external per-frame
    // state into an otherwise self-contained pass" pattern FluidSurfaceRenderPass uses for
    // SetFluidSim().
    class FxaaRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "Fxaa"; }
        std::string_view PhaseTag() const override { return "PostProcess"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        // Same shape as RenderTargetPool's SrvAllocFn: allocates `count` descriptors from
        // the renderer's shared shader-visible heap.
        using SrvAllocFn = std::function<bool(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)>;

        // (Re)creates the pass's private LDR copy texture if the requested size differs from
        // what is already allocated. Cheap no-op otherwise. Must be called once per frame,
        // before the render graph executes, since Execute() has no device access.
        //
        // allocateSrv hands out one descriptor from the renderer's main heap, and it is
        // requested only once across the pass's lifetime: that allocator bumps, so asking
        // again on every resize would leak a descriptor per resize.
        bool EnsureResources(GraphicsDevice& device, const SrvAllocFn& allocateSrv, UINT width, UINT height);

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // Current frame's back-buffer resource, copied from at the top of Execute() so FXAA
        // can sample the tone-mapped color while it is itself writing BackBuffer.
        void SetBackBufferResource(ID3D12Resource* backBuffer) { m_backBufferResource = backBuffer; }

    private:
        Resource m_inputCopyTexture;
        // The input-copy SRV lives in the renderer's main shader-visible heap, not a heap of
        // this pass's own. A private heap meant Execute() had to call SetDescriptorHeaps, and
        // the draw that followed was validated against the shared root signature: the table
        // slots left bound by earlier passes still pointed into the main heap, so the debug
        // layer reported "the descriptor heap containing handle X is different from currently
        // set descriptor heap Y" on every frame. Sharing the one heap removes the switch.
        CpuDescriptorHandle m_inputCopySrvCpu{};
        GpuDescriptorHandle m_inputCopySrv{};
        bool m_srvAllocated = false;
        UINT m_width = 0;
        UINT m_height = 0;
        DXGI_FORMAT m_inputFormat = DXGI_FORMAT_UNKNOWN;
        bool m_enabled = true;
        ID3D12Resource* m_backBufferResource = nullptr;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
