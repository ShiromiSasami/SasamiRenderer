#pragma once

#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiEngine::Render
{
    // Snapshots SceneColor into TransmissionSceneColor immediately before the water is drawn, so
    // FluidSurfaceRenderPass has the scene behind the surface to refract.
    //
    // A pass cannot read and write the same resource -- SceneColor is the water's render target --
    // so refraction needs a copy, and this is the same copy the transparency path already performs
    // for its own transmission. The destination is deliberately that same TransmissionSceneColor
    // rather than a new render target: both consumers want exactly "the scene behind me", they just
    // want it at different points in the frame. TransparentSceneColorCopyRenderPass runs later and
    // overwrites this snapshot with one that includes the water, which is what the transparent
    // geometry behind it needs.
    //
    // Split from TransparentSceneColorCopyRenderPass instead of reusing the instance because a tag
    // identifies at most one pass in the registry, and the two copies happen at different points.
    class FluidBackdropCopyRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "FluidBackdropCopy"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
