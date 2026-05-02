#pragma once

#include "Renderer/Core/GraphicsDevice.h"
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

        // Which GPU command queue this node prefers to execute on.
        // Nodes that only dispatch compute shaders (no rasterization) may return Compute
        // to allow the render graph to route them to the async compute queue.
        virtual CommandQueueType PreferredQueue() const { return CommandQueueType::Graphics; }

        virtual void BuildRequirements(RenderNodeRequirementBuilder& builder) const = 0;
        virtual void Setup(RenderGraphBuilder& builder) const = 0;
        virtual bool Execute(const RenderNodeContextView& context) const = 0;
    };
}
