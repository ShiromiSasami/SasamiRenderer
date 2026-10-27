#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiEngine::Render
{
    // Editor "Wireframe" view mode: redraws the opaque vertex-path items with a wireframe-fill PSO
    // into SceneColor, on top of the lit image (Blender's wireframe overlay rather than Unreal's
    // edges-only Wireframe view). Depth-tested against SceneDepth without writing it, pulled toward
    // the camera by a depth bias so edges on the visible surface are not lost to z-fighting.
    // Skinned meshes, transparent items, the crowd and mesh-shader-only geometry are not drawn.
    // Disabled by default; the PSO exists on the DX12 path only.
    class WireframeOverlayRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "WireframeOverlay"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        bool IsEnabled() const { return m_enabled; }
        void SetEnabled(bool enabled) { m_enabled = enabled; }

    private:
        void Execute(IRhiCommandEncoder* enc,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     const Rect& scissorRect,
                     const std::function<void()>& drawCallback) const;

        bool m_enabled = false;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
