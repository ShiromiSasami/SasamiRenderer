#include "Renderer/Passes/TransparentRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void TransparentRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawTransparentItems();
    }

    void TransparentRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool TransparentRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderNodeExecutionPolicy& policy = context.Policy();
        if (!policy.executeOpaqueFamilyPasses) {
            return true;
        }
        const RenderNodeFrameInputs& inputs = context.Inputs();
        const RenderNodeExecutionServices& services = context.Services();
        if (services.copySceneColorForTransmission &&
            !services.copySceneColorForTransmission()) {
            DebugLog("TransparentRenderNode::Execute: transmission scene color copy failed.\n");
            return false;
        }

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

    void TransparentRenderNode::Execute(IRhiCommandEncoder* enc,
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
