#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiRenderer
{
    class TransparentCompositeRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "TransparentComposite"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;
    };
}
