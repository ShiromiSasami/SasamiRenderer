#pragma once

#include <string_view>

namespace SasamiRenderer
{
    class RenderGraphBuilder;
    class RenderNodeContextView;
    class RenderNodeRequirementBuilder;

    class IRenderNode
    {
    public:
        virtual ~IRenderNode() = default;

        virtual std::string_view Tag() const { return {}; }
        virtual std::string_view PhaseTag() const { return {}; }
        virtual void BuildRequirements(RenderNodeRequirementBuilder& builder) const = 0;
        virtual void Setup(RenderGraphBuilder& builder) const = 0;
        virtual bool Execute(const RenderNodeContextView& context) const = 0;
    };
}
