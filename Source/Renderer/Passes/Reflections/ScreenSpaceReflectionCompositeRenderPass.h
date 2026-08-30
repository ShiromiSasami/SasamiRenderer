#pragma once

#include "Renderer/Passes/Core/IRenderPass.h"

namespace SasamiRenderer
{
    class ScreenSpaceReflectionCompositeRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "ScreenSpaceReflectionComposite"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;
    };
}
