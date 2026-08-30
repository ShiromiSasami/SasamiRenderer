#include "Renderer/Passes/Lighting/SSAORenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void SSAORenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void SSAORenderPass::Setup(RenderGraphBuilder& builder) const
    {
        // Declaring Read("SceneDepth") makes the render graph auto-transition it to
        // PIXEL_SHADER_RESOURCE before Execute() runs (RenderGraph::PreparePassResources).
        builder.Read("SceneDepth");
        builder.Read("GBufferNormal");
        // Declares SSAORenderPass as the graph producer for SSAOBlurRenderPass's Read("SSAO").
        // UseColorTarget makes the graph own SSAO's RTV<->SRV transitions.
        builder.Write("SSAO");
        builder.UseColorTarget("SSAO");
        builder.DependsOnPrevious();
    }

    bool SSAORenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied())
        {
            DebugLog("SSAORenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();

        if (!inputs.ao.ssaoResource || inputs.ao.ssaoRtv.ptr == 0 || inputs.ao.ssaoCbGpu == 0)
        {
            // SSAO resources not ready  Eskip silently
            return true;
        }

        // Pass 1: raw SSAO
        Execute(inputs.execution.cmdList,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                inputs.gbuffer.depthSrv,
                inputs.gbuffer.normalSrv,
                inputs.ao.ssaoRtv,
                inputs.ao.ssaoResource,
                inputs.ao.ssaoCbGpu);

        return true;
    }

    void SSAORenderPass::Execute(CommandList*              cmdList,
                                  RenderPipelineStateCache& pipelineStateCache,
                                  DescriptorHeap&           srvHeap,
                                  const Viewport&           viewport,
                                  GpuDescriptorHandle       depthSrv,
                                  GpuDescriptorHandle       normalSrv,
                                  CpuDescriptorHandle       ssaoRtv,
                                  ID3D12Resource*           ssaoResource,
                                  D3D12_GPU_VIRTUAL_ADDRESS ssaoCbGpu) const
    {
        if (!cmdList || !ssaoResource)
        {
            return;
        }

        // SSAO is graph-imported now, so PreparePassResources owns its RTV/SRV transitions;
        // the manual barrier here would double-transition.

        // --- Clear SSAO RTV to white (1.0 = no occlusion) ---
        const FLOAT clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(ssaoRtv, clearColor, 0, nullptr);

        // Render target binding is handled by PreparePassResources for this pass's
        // UseColorTarget("SSAO") declaration.

        // --- Set SSAO root signature and PSO ---
        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetSsaoRootSignature());
        cmdList->SetPipelineState(pipelineStateCache.GetSsaoPipelineState());

        // Ensure the SRV heap is bound (already set by the render loop, but set explicitly)
        DescriptorHeap* heaps[] = { &srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);

        // Bind: [0] depth SRV (t0), [1] normal SRV (t1), [2] SSAO CB (b0)
        cmdList->SetGraphicsRootDescriptorTable(0, depthSrv);
        cmdList->SetGraphicsRootDescriptorTable(1, normalSrv);
        cmdList->SetGraphicsRootConstantBufferView(2, ssaoCbGpu);

        // --- Viewport and scissor ---
        cmdList->RSSetViewports(1, &viewport);
        D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport.Width), static_cast<LONG>(viewport.Height) };
        cmdList->RSSetScissorRects(1, &scissor);

        // --- Full-screen triangle (no vertex buffer) ---
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 0, nullptr);
        cmdList->IASetIndexBuffer(nullptr);
#if defined(_DEBUG)
        DebugIncrementDrawCount();
#endif
        cmdList->DrawInstanced(3, 1, 0, 0);

        // SSAO is graph-imported now, so PreparePassResources owns its RTV/SRV transitions;
        // the manual barrier here would double-transition.
    }

    void SSAOBlurRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void SSAOBlurRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        // Read("SceneDepth") makes the render graph auto-transition it to
        // PIXEL_SHADER_RESOURCE before ExecuteBlur() runs. Read("SSAO") makes the graph
        // transition the raw SSAO texture to PIXEL_SHADER_RESOURCE for sampling here.
        builder.Read("SceneDepth");
        builder.Read("SSAO");
        // SSAO_Blur_PS.hlsl samples the GBuffer normal at register(t2) for depth/normal-aware blur.
        builder.Read("GBufferNormal");
        builder.Write("SSAOBlur");
        // UseColorTarget makes the graph own SSAOBlur's RTV<->SRV transitions.
        builder.UseColorTarget("SSAOBlur");
        builder.DependsOnPrevious();
    }

    bool SSAOBlurRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("SSAOBlurRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        if (!inputs.ao.ssaoBlurResource || inputs.ao.ssaoBlurRtv.ptr == 0 || inputs.ao.ssaoRawSrv.ptr == 0 || inputs.ao.ssaoCbGpu == 0) {
            return true;
        }

        ExecuteBlur(inputs.execution.cmdList,
                    *inputs.execution.pipelineStateCache,
                    *inputs.execution.srvHeap,
                    *inputs.execution.viewport,
                    inputs.ao.ssaoRawSrv,
                    inputs.gbuffer.depthSrv,
                    inputs.gbuffer.normalSrv,
                    inputs.ao.ssaoBlurRtv,
                    inputs.ao.ssaoBlurResource,
                    inputs.ao.ssaoCbGpu);
        return true;
    }

    void SSAOBlurRenderPass::ExecuteBlur(CommandList*              cmdList,
                                         RenderPipelineStateCache& pipelineStateCache,
                                         DescriptorHeap&           srvHeap,
                                         const Viewport&           viewport,
                                         GpuDescriptorHandle       ssaoRawSrv,
                                         GpuDescriptorHandle       depthSrv,
                                         GpuDescriptorHandle       normalSrv,
                                         CpuDescriptorHandle       ssaoBlurRtv,
                                         ID3D12Resource*           ssaoBlurResource,
                                         D3D12_GPU_VIRTUAL_ADDRESS ssaoCbGpu) const
    {
        if (!cmdList || !ssaoBlurResource)
        {
            return;
        }

        // SSAOBlur is graph-imported now, so PreparePassResources owns its RTV/SRV
        // transitions; the manual barrier here would double-transition.

        // Render target binding is handled by PreparePassResources for this pass's
        // UseColorTarget("SSAOBlur") declaration.

        // Set blur root signature and PSO
        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetSsaoBlurRootSignature());
        cmdList->SetPipelineState(pipelineStateCache.GetSsaoBlurPipelineState());

        DescriptorHeap* heaps[] = { &srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);

        // [0] t0 = raw SSAO SRV, [1] t1 = depth SRV, [2] t2 = normal SRV, [3] b0 = SSAO CB
        cmdList->SetGraphicsRootDescriptorTable(0, ssaoRawSrv);
        cmdList->SetGraphicsRootDescriptorTable(1, depthSrv);
        cmdList->SetGraphicsRootDescriptorTable(2, normalSrv);
        cmdList->SetGraphicsRootConstantBufferView(3, ssaoCbGpu);

        cmdList->RSSetViewports(1, &viewport);
        D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport.Width), static_cast<LONG>(viewport.Height) };
        cmdList->RSSetScissorRects(1, &scissor);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 0, nullptr);
        cmdList->IASetIndexBuffer(nullptr);
#if defined(_DEBUG)
        DebugIncrementDrawCount();
#endif
        cmdList->DrawInstanced(3, 1, 0, 0);

        // SSAOBlur is graph-imported now, so PreparePassResources owns its RTV/SRV
        // transitions; the manual barrier here would double-transition.
    }
}
