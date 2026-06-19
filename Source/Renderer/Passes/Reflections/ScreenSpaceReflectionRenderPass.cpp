#include "Renderer/Passes/Reflections/ScreenSpaceReflectionRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "d3dx12.h"

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

    void ScreenSpaceReflectionRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void ScreenSpaceReflectionRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Read("SceneDepth");
        builder.Read("GBufferNormal");
        builder.Read("GBufferMaterial");
        builder.Write("SceneColor");
        builder.Write("SSRSceneColorCopy");
        builder.Write("SSRReflection");
        builder.DependsOnPrevious();
    }

    bool ScreenSpaceReflectionRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("ScreenSpaceReflectionRenderPass::Execute: runtime context is invalid.\n");
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
        if (!cmdList || !pipelineStateCache || !srvHeap || !frameCoordinator || !frame ||
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

        D3D12_RESOURCE_BARRIER preCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.ssr.sceneColorResource,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.ssr.sceneColorCopyResource,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmdList->ResourceBarrier(_countof(preCopy), preCopy);
        cmdList->Get()->CopyResource(inputs.ssr.sceneColorCopyResource, inputs.ssr.sceneColorResource);

        D3D12_RESOURCE_BARRIER toCompute[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.ssr.sceneColorResource,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.ssr.sceneColorCopyResource,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.ssr.reflectionResource,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.gbuffer.depthResource,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.gbuffer.normalResource,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.gbuffer.materialResource,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cmdList->ResourceBarrier(_countof(toCompute), toCompute);

        const float identity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        const float screenParams[4] = {
            static_cast<float>(width),
            static_cast<float>(height),
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height),
        };
        const float traceParams0[4] = {
            28.0f,   // max trace distance in world units
            0.18f,   // depth thickness in view-space units
            48.0f,   // max ray-march steps
            0.78f,   // roughness where SSR fully fades out
        };
        const float traceParams1[4] = {
            4.0f,    // binary refinement steps
            0.075f,  // screen edge fade width
            0.02f,   // normal offset
            1.0f,    // reflection intensity
        };
        const float* cameraPosPtr = inputs.camera.pos;
        const float cameraPos[4] = {
            cameraPosPtr ? cameraPosPtr[0] : 0.0f,
            cameraPosPtr ? cameraPosPtr[1] : 0.0f,
            cameraPosPtr ? cameraPosPtr[2] : 0.0f,
            0.0f,
        };
        const RhiGpuAddress ssrCbGpu =
            frameCoordinator->PushCameraCB(*frame,
                                           inputs.camera.pv,
                                           inputs.camera.invPv,
                                           screenParams,
                                           traceParams0,
                                           traceParams1,
                                           cameraPos,
                                           identity,
                                           nullptr,
                                           nullptr);
        if (ssrCbGpu == 0) {
            return true;
        }

        cmdList->SetComputeRootSignature(pipelineStateCache->GetScreenSpaceReflectionRootSignature());
        cmdList->SetPipelineState(pipelineStateCache->GetScreenSpaceReflectionPipelineState());
        DescriptorHeap* heaps[] = { srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootDescriptorTable(0, inputs.gbuffer.depthSrv);
        cmdList->SetComputeRootDescriptorTable(1, inputs.gbuffer.normalSrv);
        cmdList->SetComputeRootDescriptorTable(2, inputs.gbuffer.materialSrv);
        cmdList->SetComputeRootDescriptorTable(3, inputs.ssr.sceneColorCopySrv);
        cmdList->SetComputeRootDescriptorTable(4, inputs.ssr.reflectionUav);
        cmdList->SetComputeRootConstantBufferView(5, ssrCbGpu);

        cmdList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER toComposite[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.ssr.sceneColorCopyResource,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.ssr.reflectionResource,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.gbuffer.depthResource,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.gbuffer.normalResource,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.gbuffer.materialResource,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmdList->ResourceBarrier(_countof(toComposite), toComposite);

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
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);

        return true;
    }
}
