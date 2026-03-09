#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

#include <functional>

namespace SasamiRenderer
{
    class LightingRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "Lighting"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        void Execute(CommandList* cmdList,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     const Rect& scissorRect,
                     bool useTessellation,
                     GpuDescriptorHandle shadowSrv,
                     GpuDescriptorHandle lightSrvTable,
                     GpuDescriptorHandle iblSrvTable,
                     GpuDescriptorHandle aoSrv,
                     D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                     const std::function<void()>& drawCallback) const;
    };
}
