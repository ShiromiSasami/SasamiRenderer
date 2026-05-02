#include "Renderer/Passes/TransparentRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void TransparentRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
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

        Execute(inputs.cmdList,
                *inputs.pipelineStateCache,
                *inputs.srvHeap,
                *inputs.viewport,
                *inputs.scissorRect,
                inputs.shadowSrv,
                inputs.spotShadowSrv,
                inputs.lightSrvTable,
                inputs.iblSrvTable,
                inputs.aoSrv,
                inputs.reflectionSrv,
                inputs.lightCbGpu,
                services.drawTransparentItems);
        return true;
    }

    void TransparentRenderNode::Execute(CommandList* cmdList,
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
                                        const std::function<void()>& drawCallback) const
    {
        if (!cmdList) {
            return;
        }

        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetRootSignature());
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);
        cmdList->SetPipelineState(pipelineStateCache.GetTransparentBasicPipelineState());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        DescriptorHeap* heaps[] = { &srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, shadowSrv);
        cmdList->SetGraphicsRootDescriptorTable(4, lightSrvTable);
        cmdList->SetGraphicsRootDescriptorTable(5, iblSrvTable);
        cmdList->SetGraphicsRootDescriptorTable(6, aoSrv);
        cmdList->SetGraphicsRootDescriptorTable(7, reflectionSrv);
        cmdList->SetGraphicsRootDescriptorTable(12, spotShadowSrv);

        if (lightCbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(3, lightCbGpu);
        }

        if (drawCallback) {
            drawCallback();
        }
    }
}
