#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiRenderer
{
    class SSAORenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "SSAO"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

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
    };

    class SSAOBlurRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "SSAOBlur"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

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
