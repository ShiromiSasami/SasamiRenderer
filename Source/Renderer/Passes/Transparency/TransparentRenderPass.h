#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiRenderer
{
    class TransparentRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "Transparent"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

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
                     GpuDescriptorHandle transmissionSceneColorSrv,
                     D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                     const std::function<void()>& drawCallback) const;
    };
}
