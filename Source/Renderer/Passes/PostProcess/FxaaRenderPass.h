#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiRenderer
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

        // (Re)creates the pass's private LDR copy texture and SRV heap if the requested
        // size differs from what is already allocated. Cheap no-op otherwise. Must be
        // called once per frame, before the render graph executes, since Execute() has no
        // device access.
        bool EnsureResources(GraphicsDevice& device, UINT width, UINT height);

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // Current frame's back-buffer resource, copied from at the top of Execute() so FXAA
        // can sample the tone-mapped color while it is itself writing BackBuffer.
        void SetBackBufferResource(ID3D12Resource* backBuffer) { m_backBufferResource = backBuffer; }

    private:
        Resource m_inputCopyTexture;
        // Execute() is logically const but must bind this heap on the command list,
        // and SetDescriptorHeaps takes a non-const pointer: the heap is GPU-side
        // state, not part of the pass's logical value.
        mutable DescriptorHeap m_srvHeap;
        GpuDescriptorHandle m_inputCopySrv{};
        UINT m_width = 0;
        UINT m_height = 0;
        bool m_enabled = true;
        ID3D12Resource* m_backBufferResource = nullptr;
    };
}
