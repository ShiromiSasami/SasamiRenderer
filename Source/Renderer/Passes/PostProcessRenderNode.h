#pragma once

#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

#include <functional>

namespace SasamiRenderer
{
    class PostProcessRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "PostProcess"; }
        std::string_view PhaseTag() const override { return "PostProcess"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        void Execute(const std::function<void()>& ensureSceneTargetsPrepared) const;
    };
}
