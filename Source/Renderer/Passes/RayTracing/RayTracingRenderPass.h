#pragma once

#include "Renderer/Passes/Core/IRenderPass.h"

namespace SasamiRenderer
{
    class RayTracingRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "RayTracing"; }
        std::string_view PhaseTag() const override { return "Lighting"; }

        // Hardware ray tracing copies the resolved output into the swapchain,
        // so it must run on the graphics/direct queue rather than async compute.
        CommandQueueType PreferredQueue() const override { return CommandQueueType::Graphics; }

        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;
    };
}
