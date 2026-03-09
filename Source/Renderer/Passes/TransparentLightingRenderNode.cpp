#include "Renderer/Passes/TransparentLightingRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void TransparentLightingRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
    }

    void TransparentLightingRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool TransparentLightingRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentLightingRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderNodeExecutionPolicy& policy = context.Policy();
        if (!policy.executeLightingFamilyPasses) {
            return true;
        }
        const RenderNodeFrameInputs& inputs = context.Inputs();
        const RenderNodeExecutionServices& services = context.Services();

        Execute(inputs.cmdList,
                *inputs.pipelineStateCache,
                *inputs.srvHeap,
                *inputs.viewport,
                *inputs.scissorRect,
                false,
                inputs.shadowSrv,
                inputs.lightSrvTable,
                inputs.iblSrvTable,
                inputs.aoSrv,
                inputs.lightCbGpu,
                services.drawTransparentItems);
        return true;
    }

    void TransparentLightingRenderNode::Execute(CommandList* cmdList,
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
                                                const std::function<void()>& drawCallback) const
    {
        if (!cmdList) {
            return;
        }

        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetRootSignature());
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        if (useTessellation) {
            cmdList->SetPipelineState(pipelineStateCache.GetTessellationPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        } else {
            cmdList->SetPipelineState(pipelineStateCache.GetPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

        DescriptorHeap* heaps[] = { &srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, shadowSrv);
        cmdList->SetGraphicsRootDescriptorTable(4, lightSrvTable);
        cmdList->SetGraphicsRootDescriptorTable(5, iblSrvTable);
        cmdList->SetGraphicsRootDescriptorTable(6, aoSrv);

        if (lightCbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(3, lightCbGpu);
        }

        if (drawCallback) {
            drawCallback();
        }
    }
}
