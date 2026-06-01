#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

#include <functional>

namespace SasamiRenderer
{
    class OpaqueRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "Opaque"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        void Execute(IRhiCommandEncoder* enc,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     const Rect& scissorRect,
                     GpuDescriptorHandle shadowSrv,
                     GpuDescriptorHandle spotShadowSrv,
                     GpuDescriptorHandle lightSrvTable,
                     GpuDescriptorHandle iblSrvTable,
                     GpuDescriptorHandle aoSrv,
                     GpuDescriptorHandle reflectionSrv,
                     D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                     bool useTessellation,
                     bool tessDebugColors,
                     const std::function<void()>& drawCallback) const;

    };
}
