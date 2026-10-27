#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiEngine::Render
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
                         CpuDescriptorHandle ssaoBlurRtv,
                         ID3D12Resource* ssaoBlurResource,
                         D3D12_GPU_VIRTUAL_ADDRESS ssaoCbGpu) const;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
