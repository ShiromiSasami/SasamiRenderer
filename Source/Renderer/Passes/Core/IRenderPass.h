#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include <string_view>

namespace SasamiEngine::Render
{
    class RenderGraphBuilder;
    class RenderPassContextView;
    class RenderPassRequirementBuilder;

    class IRenderPass
    {
    public:
        virtual ~IRenderPass() = default;

        virtual std::string_view Tag() const { return {}; }
        virtual std::string_view PhaseTag() const { return {}; }

        // Which GPU command queue this pass prefers to execute on.
        // Passes that only dispatch compute shaders (no rasterization) may return Compute
        // to allow the render graph to route them to the async compute queue.
        virtual CommandQueueType PreferredQueue() const { return CommandQueueType::Graphics; }

        virtual void BuildRequirements(RenderPassRequirementBuilder& builder) const = 0;
        virtual void Setup(RenderGraphBuilder& builder) const = 0;
        virtual bool Execute(const RenderPassContextView& context) const = 0;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
