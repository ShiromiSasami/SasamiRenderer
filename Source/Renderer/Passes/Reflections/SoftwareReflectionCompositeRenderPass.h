#pragma once

#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiRenderer
{
    class SoftwareReflectionCompositeRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "SoftwareReflectionComposite"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;
    };
}
