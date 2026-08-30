#include "Renderer/Passes/PostProcess/FxaaRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    bool FxaaRenderPass::EnsureResources(GraphicsDevice& device, UINT width, UINT height)
    {
        if (width == 0u || height == 0u) {
            return false;
        }
        if (m_inputCopyTexture.IsValid() && m_width == width && m_height == height) {
            return true;
        }

        m_inputCopyTexture.Reset();
        m_srvHeap.Reset();
        m_width = 0;
        m_height = 0;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);

        if (FAILED(device.CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   nullptr, m_inputCopyTexture))) {
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1u;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device.CreateDescriptorHeap(heapDesc, m_srvHeap))) {
            m_inputCopyTexture.Reset();
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_inputCopyTexture, &srvDesc, m_srvHeap.GetCPUDescriptorHandleForHeapStart());
        m_inputCopySrv = m_srvHeap.GetGPUDescriptorHandleForHeapStart();

        m_width = width;
        m_height = height;
        return true;
    }

    void FxaaRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void FxaaRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        // The private input-copy texture is filled by a manual CopyResource inside
        // Execute() (see comment there), outside the graph's own resource tracking, so
        // this pass only needs to declare its BackBuffer write.
        builder.Write("BackBuffer");
        builder.UseColorTarget("BackBuffer");
        builder.DependsOnPrevious();
    }

    bool FxaaRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("FxaaRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        if (!m_enabled) {
            // BackBuffer already holds the tone-mapped image written by PostProcessRenderPass
            // just before this pass (BackBuffer's clearColorOnFirstUse is false), so doing
            // nothing here leaves it untouched -- functionally identical to skipping this pass.
            return true;
        }
        if (!m_inputCopyTexture.IsValid() || !m_backBufferResource) {
            return true;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        CommandList* cmdList = inputs.execution.cmdList;
        RenderPipelineStateCache* pipelineStateCache = inputs.execution.pipelineStateCache;
        if (!cmdList || !pipelineStateCache || !inputs.execution.viewport || !inputs.execution.scissorRect ||
            !inputs.execution.frameCoordinator || !inputs.execution.frame) {
            return false;
        }

        // BackBuffer arrives here already transitioned to RENDER_TARGET and bound by
        // RenderGraph::PreparePassResources (this pass declared UseColorTarget("BackBuffer")).
        // FXAA needs to sample the very same tone-mapped pixels it is about to overwrite, so
        // copy them into a private texture first (same pattern as
        // Renderer::CopySceneColorForTransmission).
        D3D12_RESOURCE_BARRIER preCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_backBufferResource,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_inputCopyTexture.Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmdList->ResourceBarrier(_countof(preCopy), preCopy);
        cmdList->Get()->CopyResource(m_inputCopyTexture.Get(), m_backBufferResource);

        D3D12_RESOURCE_BARRIER postCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_inputCopyTexture.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_backBufferResource,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmdList->ResourceBarrier(_countof(postCopy), postCopy);

        cmdList->SetGraphicsRootSignature(pipelineStateCache->GetRootSignature());
        cmdList->SetPipelineState(pipelineStateCache->GetFxaaPipelineState());
        cmdList->RSSetViewports(1, inputs.execution.viewport);
        cmdList->RSSetScissorRects(1, inputs.execution.scissorRect);

        DescriptorHeap* heaps[] = { &m_srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(0, m_inputCopySrv);

        static const float kIdentity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        const float fxaaParams[4] = {
            m_width  > 0u ? 1.0f / static_cast<float>(m_width)  : 0.0f,
            m_height > 0u ? 1.0f / static_cast<float>(m_height) : 0.0f,
            0.0f, 0.0f,
        };
        const D3D12_GPU_VIRTUAL_ADDRESS fxaaCbGpu = inputs.execution.frameCoordinator->PushCameraCB(
            *inputs.execution.frame, kIdentity, kIdentity, fxaaParams, nullptr, nullptr);
        if (fxaaCbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(2, fxaaCbGpu);
        }

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 0, nullptr);
        cmdList->IASetIndexBuffer(nullptr);
#if defined(_DEBUG)
        DebugIncrementDrawCount();
#endif
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);
        return true;
    }
}
