#include "Renderer/Passes/OpaqueRenderNode.h"
#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void OpaqueRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
    }

    void OpaqueRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Write("SceneColor");
        builder.Write("SceneDepth");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool OpaqueRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("OpaqueRenderNode::Execute: runtime context is invalid.\n");
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
                policy.useTessellation,
                policy.useTessellationDebugColors,
                services.drawOpaqueItems);
        return true;
    }

    void OpaqueRenderNode::Execute(CommandList* cmdList,
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
                                   const std::function<void()>& drawCallback) const
    {
        if (!cmdList) {
            return;
        }

        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetRootSignature());
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);
        if (useTessellation) {
            if (tessDebugColors && pipelineStateCache.GetTessellationDebugPipelineState().Get()) {
                cmdList->SetPipelineState(pipelineStateCache.GetTessellationDebugPipelineState());
            } else {
                cmdList->SetPipelineState(pipelineStateCache.GetTessellationPipelineState());
            }
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        } else {
            cmdList->SetPipelineState(pipelineStateCache.GetBasicPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

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
