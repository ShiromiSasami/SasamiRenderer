#include "Renderer/Passes/Reflections/ScreenSpaceReflectionCompositeRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"

#include <cstdint>

namespace SasamiRenderer
{
    namespace
    {
        bool UsesScreenSpaceReflectionDebugView(RendererEnums::GBufferDebugView view)
        {
            return view == RendererEnums::GBufferDebugView::ReflectionRadiance ||
                   view == RendererEnums::GBufferDebugView::ReflectionAlpha;
        }
    }

    void ScreenSpaceReflectionCompositeRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
        builder.RequireSkybox();
    }

    void ScreenSpaceReflectionCompositeRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("GBufferNormal");
        builder.Read("GBufferMaterial");
        builder.Read("SSRReflection");
        builder.Write("SceneColor");
        // ScreenSpaceReflectionRenderPass (the trace pass) leaves SceneColor in RENDER_TARGET,
        // so this UseColorTarget is a no-op transition that keeps the graph's bookkeeping in
        // sync rather than driving an actual barrier.
        builder.UseColorTarget("SceneColor");
        builder.DependsOnPrevious();
    }

    bool ScreenSpaceReflectionCompositeRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("ScreenSpaceReflectionCompositeRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassExecutionPolicy& policy = context.Policy();
        if (!policy.useScreenSpaceReflections ||
            policy.useSoftwareRayTracedReflections ||
            (policy.gBufferDebugView != RendererEnums::GBufferDebugView::FinalLit &&
             !UsesScreenSpaceReflectionDebugView(policy.gBufferDebugView))) {
            return true;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        CommandList* cmdList = inputs.execution.cmdList;
        RenderPipelineStateCache* pipelineStateCache = inputs.execution.pipelineStateCache;
        DescriptorHeap* srvHeap = inputs.execution.srvHeap;
        RendererFrameCoordinator* frameCoordinator = inputs.execution.frameCoordinator;
        RendererFrameCoordinator::FrameContext* frame = inputs.execution.frame;
        // Mirrors ScreenSpaceReflectionRenderPass::Execute's guard exactly (not just the fields
        // this composite draw reads) so this pass only ever runs on frames where the trace pass
        // also ran and populated SSRReflection.
        if (!cmdList || !pipelineStateCache || !srvHeap || !frameCoordinator || !frame ||
            !inputs.camera.invPv ||
            !inputs.ssr.sceneColorResource ||
            !inputs.ssr.sceneColorCopyResource ||
            !inputs.ssr.reflectionResource ||
            !inputs.gbuffer.depthResource ||
            !inputs.gbuffer.normalResource ||
            !inputs.gbuffer.materialResource ||
            inputs.ssr.sceneColorCopySrv.ptr == 0 ||
            inputs.ssr.reflectionSrv.ptr == 0 ||
            inputs.ssr.reflectionUav.ptr == 0 ||
            inputs.gbuffer.depthSrv.ptr == 0 ||
            inputs.gbuffer.normalSrv.ptr == 0 ||
            inputs.gbuffer.materialSrv.ptr == 0 ||
            inputs.lighting.lightCbGpu == 0) {
            return true;
        }

        const uint32_t width = policy.renderWidth;
        const uint32_t height = policy.renderHeight;
        if (width == 0u || height == 0u) {
            return true;
        }

        cmdList->SetGraphicsRootSignature(pipelineStateCache->GetRootSignature());
        cmdList->SetPipelineState(pipelineStateCache->GetSwrtReflectionCompositePipelineState());
        cmdList->RSSetViewports(1, inputs.execution.viewport);
        cmdList->RSSetScissorRects(1, inputs.execution.scissorRect);

        CpuDescriptorHandle rtv = inputs.ssr.sceneColorRtv;
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmdList->SetGraphicsRootDescriptorTable(0, inputs.gbuffer.albedoSrv);
        cmdList->SetGraphicsRootConstantBufferView(3, inputs.lighting.lightCbGpu);
        cmdList->SetGraphicsRootDescriptorTable(6, inputs.gbuffer.materialSrv);
        cmdList->SetGraphicsRootDescriptorTable(7, inputs.ssr.reflectionSrv);
        cmdList->SetGraphicsRootDescriptorTable(8, inputs.gbuffer.normalSrv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
#if defined(_DEBUG)
        DebugIncrementDrawCount();
#endif
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);

        return true;
    }
}
