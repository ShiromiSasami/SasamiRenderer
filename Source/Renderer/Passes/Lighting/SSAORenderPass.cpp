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
        // Declare read on SceneDepth so the render graph orders SSAO after the opaque pass.
        // Resource state transitions for depth and the SSAO render target are handled
        // manually in Execute() using explicit D3D12 barriers.
        builder.Read("SceneDepth");
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
        const RenderPassExecutionPolicy& policy = context.Policy();

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
                inputs.gbuffer.depthResource,
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
                                  ID3D12Resource*           depthResource,
                                  CpuDescriptorHandle       ssaoRtv,
                                  ID3D12Resource*           ssaoResource,
                                  D3D12_GPU_VIRTUAL_ADDRESS ssaoCbGpu) const
    {
        if (!cmdList || !ssaoResource)
        {
            return;
        }

        // --- Barriers: transition both SSAO texture (SRV->RTV) and depth (DEPTH_WRITE->SRV) ---
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        UINT barrierCount = 0;

        barriers[barrierCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[barrierCount].Transition.pResource   = ssaoResource;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[barrierCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++barrierCount;

        if (depthResource)
        {
            barriers[barrierCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[barrierCount].Transition.pResource   = depthResource;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barriers[barrierCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++barrierCount;
        }

        cmdList->ResourceBarrier(barrierCount, barriers);

        // --- Clear SSAO RTV to white (1.0 = no occlusion) ---
        const FLOAT clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(ssaoRtv, clearColor, 0, nullptr);

        // --- Bind SSAO render target (no depth) ---
        cmdList->OMSetRenderTargets(1, &ssaoRtv, FALSE, nullptr);

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
        cmdList->DrawInstanced(3, 1, 0, 0);

        // --- Barriers: SSAO texture (RTV->SRV) and depth (SRV->DEPTH_WRITE) ---
        D3D12_RESOURCE_BARRIER endBarriers[2] = {};
        UINT endBarrierCount = 0;

        endBarriers[endBarrierCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        endBarriers[endBarrierCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        endBarriers[endBarrierCount].Transition.pResource   = ssaoResource;
        endBarriers[endBarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        endBarriers[endBarrierCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        endBarriers[endBarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++endBarrierCount;

        if (depthResource)
        {
            endBarriers[endBarrierCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            endBarriers[endBarrierCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            endBarriers[endBarrierCount].Transition.pResource   = depthResource;
            endBarriers[endBarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            endBarriers[endBarrierCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            endBarriers[endBarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++endBarrierCount;
        }

        cmdList->ResourceBarrier(endBarrierCount, endBarriers);
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
        // The D3D12 resources are still transitioned manually in ExecuteBlur().
        // The graph declaration keeps this pass ordered after raw SSAO.
        builder.Read("SceneDepth");
        builder.Read("SSAO");
        builder.Write("SSAOBlur");
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
                    inputs.gbuffer.depthResource,
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
                                         ID3D12Resource*           depthResource,
                                         CpuDescriptorHandle       ssaoBlurRtv,
                                         ID3D12Resource*           ssaoBlurResource,
                                         D3D12_GPU_VIRTUAL_ADDRESS ssaoCbGpu) const
    {
        if (!cmdList || !ssaoBlurResource)
        {
            return;
        }

        // After the raw SSAO pass: ssaoBlurTexture is in SRV state, depth is in DEPTH_WRITE.
        // We need: ssaoBlurTexture ↁERTV, depth ↁESRV (for edge-stopping bilateral).
        D3D12_RESOURCE_BARRIER beginBarriers[2] = {};
        UINT beginCount = 0;

        beginBarriers[beginCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        beginBarriers[beginCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        beginBarriers[beginCount].Transition.pResource   = ssaoBlurResource;
        beginBarriers[beginCount].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        beginBarriers[beginCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        beginBarriers[beginCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++beginCount;

        if (depthResource)
        {
            beginBarriers[beginCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            beginBarriers[beginCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            beginBarriers[beginCount].Transition.pResource   = depthResource;
            beginBarriers[beginCount].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            beginBarriers[beginCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            beginBarriers[beginCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++beginCount;
        }

        cmdList->ResourceBarrier(beginCount, beginBarriers);

        // Bind blur render target
        cmdList->OMSetRenderTargets(1, &ssaoBlurRtv, FALSE, nullptr);

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
        cmdList->DrawInstanced(3, 1, 0, 0);

        // Restore states: ssaoBlurTexture ↁESRV, depth ↁEDEPTH_WRITE
        D3D12_RESOURCE_BARRIER endBarriers[2] = {};
        UINT endCount = 0;

        endBarriers[endCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        endBarriers[endCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        endBarriers[endCount].Transition.pResource   = ssaoBlurResource;
        endBarriers[endCount].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        endBarriers[endCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        endBarriers[endCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++endCount;

        if (depthResource)
        {
            endBarriers[endCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            endBarriers[endCount].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            endBarriers[endCount].Transition.pResource   = depthResource;
            endBarriers[endCount].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            endBarriers[endCount].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            endBarriers[endCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++endCount;
        }

        cmdList->ResourceBarrier(endCount, endBarriers);
    }
}
