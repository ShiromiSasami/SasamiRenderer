#include "Renderer/Passes/Geometry/OpaqueRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    namespace
    {
        void ExecuteSkinnedOpaquePass(IRhiCommandEncoder* enc,
                                      RenderPipelineStateCache& pipelineStateCache,
                                      DescriptorHeap& srvHeap,
                                      GpuDescriptorHandle shadowSrv,
                                      GpuDescriptorHandle spotShadowSrv,
                                      GpuDescriptorHandle vsmSrv,
                                      GpuDescriptorHandle lightSrvTable,
                                      GpuDescriptorHandle iblSrvTable,
                                      GpuDescriptorHandle aoSrv,
                                      GpuDescriptorHandle reflectionSrv,
                                      GpuDescriptorHandle transparentBackfaceDistanceSrv,
                                      D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                      const std::function<void()>& drawSkinnedCallback)
        {
            if (!enc || !drawSkinnedCallback) return;

            enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetSkinnedRootSignature()));
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetSkinnedPipelineState()));

            enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
            enc->SetGraphicsDescriptorTable(1,  { shadowSrv.ptr });
            enc->SetGraphicsDescriptorTable(4,  { lightSrvTable.ptr });
            enc->SetGraphicsDescriptorTable(5,  { iblSrvTable.ptr });
            enc->SetGraphicsDescriptorTable(6,  { aoSrv.ptr });
            enc->SetGraphicsDescriptorTable(7,  { reflectionSrv.ptr });
            enc->SetGraphicsDescriptorTable(12, { spotShadowSrv.ptr });
            enc->SetGraphicsDescriptorTable(13, { vsmSrv.ptr });
            enc->SetGraphicsDescriptorTable(14, { transparentBackfaceDistanceSrv.ptr });
            if (lightCbGpu != 0) {
                enc->SetGraphicsConstantBufferView(3, lightCbGpu);
            }
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

            drawSkinnedCallback();
        }
    }

    void OpaqueRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
    }

    void OpaqueRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("ShadowMap");
        builder.Write("SceneColor");
        builder.Write("SceneDepth");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool OpaqueRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("OpaqueRenderPass::Execute: runtime context is invalid.\n");
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
                inputs.reflectionSrv,
                inputs.lighting.lightCbGpu,
                policy.useTessellation,
                policy.useTessellationDebugColors,
                services.drawOpaqueItems);

        if (services.drawSkinnedOpaqueItems) {
            ExecuteSkinnedOpaquePass(inputs.execution.commandEncoder,
                                     *inputs.execution.pipelineStateCache,
                                     *inputs.execution.srvHeap,
                                     inputs.shadow.shadowSrv,
                                     inputs.shadow.spotShadowSrv,
                                     inputs.shadow.vsmSrv,
                                     inputs.lighting.lightSrvTable,
                                     inputs.lighting.iblSrvTable,
                                     inputs.ao.aoSrv,
                                     inputs.reflectionSrv,
                                     inputs.transparentBackfaceDistanceSrv,
                                     inputs.lighting.lightCbGpu,
                                     services.drawSkinnedOpaqueItems);
        }
        return true;
    }

    void OpaqueRenderPass::Execute(IRhiCommandEncoder* enc,
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
        if (!enc) {
            return;
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);
        if (useTessellation) {
            if (tessDebugColors && pipelineStateCache.GetTessellationDebugPipelineState().Get()) {
                enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationDebugPipelineState()));
            } else {
                enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetTessellationPipelineState()));
            }
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::PatchList);
        } else {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetBasicPipelineState()));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);
        }

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
        enc->SetGraphicsDescriptorTable(1,  { shadowSrv.ptr });
        enc->SetGraphicsDescriptorTable(4,  { lightSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(5,  { iblSrvTable.ptr });
        enc->SetGraphicsDescriptorTable(6,  { aoSrv.ptr });
        enc->SetGraphicsDescriptorTable(7,  { reflectionSrv.ptr });
        enc->SetGraphicsDescriptorTable(12, { spotShadowSrv.ptr });

        if (lightCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(3, lightCbGpu);
        }

        if (drawCallback) {
            drawCallback();
        }
    }

}
