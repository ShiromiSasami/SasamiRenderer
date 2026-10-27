#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiEngine::Render
{
    class GBufferRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "OpaqueGBuffer"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        void Execute(IRhiCommandEncoder* enc,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     const Rect& scissorRect,
                     bool useTessellation,
                     bool useTessellationWireframe,
                     bool useTessellationDebugColors,
                     const std::function<void()>& drawCallback) const;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
