#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

namespace SasamiRenderer
{
    class SSAORenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "SSAO"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        void Execute(CommandList* cmdList,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     GpuDescriptorHandle depthSrv,
                     GpuDescriptorHandle normalSrv,
                     ID3D12Resource* depthResource,
                     CpuDescriptorHandle ssaoRtv,
                     ID3D12Resource* ssaoResource,
                     D3D12_GPU_VIRTUAL_ADDRESS ssaoCbGpu) const;

        void ExecuteBlur(CommandList* cmdList,
                         RenderPipelineStateCache& pipelineStateCache,
                         DescriptorHeap& srvHeap,
                         const Viewport& viewport,
                         GpuDescriptorHandle ssaoRawSrv,
                         GpuDescriptorHandle depthSrv,
                         GpuDescriptorHandle normalSrv,
                         ID3D12Resource* depthResource,
                         CpuDescriptorHandle ssaoBlurRtv,
                         ID3D12Resource* ssaoBlurResource,
                         D3D12_GPU_VIRTUAL_ADDRESS ssaoCbGpu) const;
    };
}
