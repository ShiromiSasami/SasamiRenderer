#include "Renderer/Passes/Transparency/TransparentLightingRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void TransparentLightingRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawTransparentItems();
    }

    void TransparentLightingRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Read("SceneDepth");
        builder.Read("TransparentBackfaceDistance");
        builder.Write("TransparentOitAccum");
        builder.Write("TransparentOitRevealage");
        builder.UseColorTarget("TransparentOitAccum");
        builder.UseColorTarget("TransparentOitRevealage");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool TransparentLightingRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("TransparentLightingRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        const RenderPassExecutionPolicy& policy = context.Policy();
        if (!policy.executeLightingFamilyPasses) {
            return true;
        }
        const RenderPassFrameInputs& inputs = context.Inputs();
        const RenderPassExecutionServices& services = context.Services();

        Execute(inputs.execution.commandEncoder,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                *inputs.execution.scissorRect,
                false,
                inputs.shadow.shadowSrv,
                inputs.shadow.spotShadowSrv,
                inputs.shadow.vsmSrv,
                inputs.lighting.lightSrvTable,
                inputs.lighting.iblSrvTable,
                inputs.ao.aoSrv,
                inputs.transmissionSceneColorSrv,
                inputs.gbuffer.depthSrv,
                inputs.transparentBackfaceDistanceSrv,
                inputs.lighting.lightCbGpu,
                services.drawTransparentItems,
                services.drawSkinnedTransparentItems);
        return true;
    }

    void TransparentLightingRenderPass::Execute(IRhiCommandEncoder* enc,
                                                RenderPipelineStateCache& pipelineStateCache,
                                                DescriptorHeap& srvHeap,
                                                const Viewport& viewport,
                                                const Rect& scissorRect,
                                                bool useTessellation,
                                                GpuDescriptorHandle shadowSrv,
                                                GpuDescriptorHandle spotShadowSrv,
                                                GpuDescriptorHandle vsmSrv,
                                                GpuDescriptorHandle lightSrvTable,
                                                GpuDescriptorHandle iblSrvTable,
                                                GpuDescriptorHandle aoSrv,
                                                GpuDescriptorHandle transmissionSceneColorSrv,
                                                GpuDescriptorHandle sceneDepthSrv,
                                                GpuDescriptorHandle transparentBackfaceDistanceSrv,
                                                D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                const std::function<void()>& drawCallback,
                                                const std::function<void()>& drawSkinnedCallback) const
    {
        if (!enc) {
            return;
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);

        if (useTessellation) {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationPipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::PatchList);
        } else {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTransparentOitPipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        }

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
        enc->SetGraphicsDescriptorTable(1,  { shadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(4,  { lightSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(5,  { iblSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(6,  { aoSrv.ptr });
        enc->SetGraphicsDescriptorTable(7,  { transmissionSceneColorSrv.ptr });
        enc->SetGraphicsDescriptorTable(11, { sceneDepthSrv.ptr });
        enc->SetGraphicsDescriptorTable(12, { spotShadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(13, { vsmSrv.ptr });
        enc->SetGraphicsDescriptorTable(14, { transparentBackfaceDistanceSrv.ptr });

        if (lightCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(3, lightCbGpu);
        }

        if (drawCallback) {
            drawCallback();
        }

        if (drawSkinnedCallback) {
            enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetSkinnedRootSignature()));
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetSkinnedTransparentOitPipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
            enc->SetGraphicsDescriptorTable(1,  { shadowSrv.ptr });
            enc->SetGraphicsDescriptorTable(4,  { lightSrvTable.ptr });
            enc->SetGraphicsDescriptorTable(5,  { iblSrvTable.ptr });
            enc->SetGraphicsDescriptorTable(6,  { aoSrv.ptr });
            enc->SetGraphicsDescriptorTable(7,  { transmissionSceneColorSrv.ptr });
            enc->SetGraphicsDescriptorTable(11, { sceneDepthSrv.ptr });
            enc->SetGraphicsDescriptorTable(12, { spotShadowSrv.ptr });
            enc->SetGraphicsDescriptorTable(13, { vsmSrv.ptr });
            enc->SetGraphicsDescriptorTable(14, { transparentBackfaceDistanceSrv.ptr });
            if (lightCbGpu != 0) {
                enc->SetGraphicsConstantBufferView(3, lightCbGpu);
            }
            drawSkinnedCallback();
        }
    }
}
