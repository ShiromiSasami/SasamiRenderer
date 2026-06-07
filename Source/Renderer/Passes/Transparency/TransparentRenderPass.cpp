#include "Renderer/Passes/Transparency/TransparentRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void TransparentRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawTransparentItems();
    }

    void TransparentRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool TransparentRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderPassExecutionPolicy& policy = context.Policy();
        if (!policy.executeOpaqueFamilyPasses) {
            return true;
        }
        const RenderPassFrameInputs& inputs = context.Inputs();
        const RenderPassExecutionServices& services = context.Services();

        Execute(inputs.execution.commandEncoder,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                *inputs.execution.scissorRect,
                inputs.shadow.shadowSrv,
                inputs.shadow.spotShadowSrv,
                inputs.lighting.lightSrvTable,
                inputs.lighting.iblSrvTable,
                inputs.ao.aoSrv,
                inputs.transmissionSceneColorSrv,
                inputs.lighting.lightCbGpu,
                services.drawTransparentItems);
        return true;
    }

    void TransparentRenderPass::Execute(IRhiCommandEncoder* enc,
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
                                        const std::function<void()>& drawCallback) const
    {
        if (!enc) {
            return;
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);
        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTransparentBasicPipelineState()));
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
        enc->SetGraphicsDescriptorTable(1,  { shadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(4,  { lightSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(5,  { iblSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(6,  { aoSrv.ptr });
        enc->SetGraphicsDescriptorTable(7,  { transmissionSceneColorSrv.ptr });
        enc->SetGraphicsDescriptorTable(12, { spotShadowSrv.ptr });

        if (lightCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(3, lightCbGpu);
        }

        if (drawCallback) {
            drawCallback();
        }
    }
}
