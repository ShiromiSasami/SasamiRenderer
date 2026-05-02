#include "Renderer/Passes/LightingRenderNode.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Core/RenderGraph.h"

namespace SasamiRenderer
{
    void LightingRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
    }

    void LightingRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Read("SceneDepth");
        builder.Write("SceneColor");
        builder.Write("GBufferAlbedo");
        builder.Write("GBufferNormal");
        builder.Write("GBufferMaterial");
        builder.Write("GBufferEmissive");
        builder.UseColorTarget("SceneColor");
        builder.UseColorTarget("GBufferAlbedo");
        builder.UseColorTarget("GBufferNormal");
        builder.UseColorTarget("GBufferMaterial");
        builder.UseColorTarget("GBufferEmissive");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool LightingRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("LightingRenderNode::Execute: runtime context is invalid.\n");
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
                policy.useTessellation,
                policy.useTessellationWireframe,
                policy.useTessellationDebugColors,
                policy.useMeshletDebugView,
                inputs.shadowSrv,
                inputs.spotShadowSrv,
                inputs.lightSrvTable,
                inputs.iblSrvTable,
                inputs.aoSrv,
                inputs.reflectionSrv,
                inputs.depthSrv,
                inputs.lightCbGpu,
                services.drawOpaqueItems);
        return true;
    }

    void LightingRenderNode::Execute(CommandList* cmdList,
                                     RenderPipelineStateCache& pipelineStateCache,
                                     DescriptorHeap& srvHeap,
                                     const Viewport& viewport,
                                     const Rect& scissorRect,
                                     bool useTessellation,
                                     bool useTessellationWireframe,
                                     bool useTessellationDebugColors,
                                     bool useMeshletDebugView,
                                     GpuDescriptorHandle shadowSrv,
                                     GpuDescriptorHandle spotShadowSrv,
                                     GpuDescriptorHandle lightSrvTable,
                                     GpuDescriptorHandle iblSrvTable,
                                     GpuDescriptorHandle aoSrv,
                                     GpuDescriptorHandle reflectionSrv,
                                     GpuDescriptorHandle depthSrv,
                                     D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                     const std::function<void()>& drawCallback) const
    {
        if (!cmdList) {
            return;
        }

        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetRootSignature());
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        if (useMeshletDebugView && pipelineStateCache.GetMeshletDebugPipelineState().Get()) {
            cmdList->SetPipelineState(pipelineStateCache.GetMeshletDebugPipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        } else if (useTessellation && useTessellationWireframe) {
            cmdList->SetPipelineState(pipelineStateCache.GetTessellationWireframePipelineState());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        } else if (useTessellation) {
            if (useTessellationDebugColors && pipelineStateCache.GetTessellationDebugPipelineState().Get()) {
                cmdList->SetPipelineState(pipelineStateCache.GetTessellationDebugPipelineState());
            } else {
                cmdList->SetPipelineState(pipelineStateCache.GetTessellationPipelineState());
            }
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
        cmdList->SetGraphicsRootDescriptorTable(7, reflectionSrv);
        cmdList->SetGraphicsRootDescriptorTable(11, depthSrv);
        cmdList->SetGraphicsRootDescriptorTable(12, spotShadowSrv);

        if (lightCbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(3, lightCbGpu);
        }

        if (drawCallback) {
            drawCallback();
        }
    }
}
