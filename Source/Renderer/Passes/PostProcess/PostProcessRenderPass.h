#pragma once

#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiRenderer
{
    class PostProcessRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "PostProcess"; }
        std::string_view PhaseTag() const override { return "PostProcess"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        void Execute(const std::function<void()>& ensureSceneTargetsPrepared) const;
    };
}
