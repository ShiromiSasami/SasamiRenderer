#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

namespace SasamiRenderer
{
    class TransparentCompositeRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "TransparentComposite"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;
    };
}
