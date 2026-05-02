#pragma once

#include "Renderer/Passes/IRenderNode.h"

namespace SasamiRenderer
{
    class RayTracingRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "RayTracing"; }
        std::string_view PhaseTag() const override { return "Lighting"; }

        // Hardware ray tracing copies the resolved output into the swapchain,
        // so it must run on the graphics/direct queue rather than async compute.
        CommandQueueType PreferredQueue() const override { return CommandQueueType::Graphics; }

        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;
    };
}
