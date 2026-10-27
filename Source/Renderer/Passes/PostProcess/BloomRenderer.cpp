#include "Renderer/Passes/PostProcess/BloomRenderer.h"

#include "d3dx12.h"

#include <algorithm>

namespace SasamiEngine::Render
{
    bool BloomRenderer::EnsureResources(GraphicsDevice& device, const SrvAllocFn& allocateSrv,
                                        UINT sceneWidth, UINT sceneHeight)
    {
        if (sceneWidth < 2u || sceneHeight < 2u) {
            return false;
        }
        if (m_mipCount > 0 && m_sceneWidth == sceneWidth && m_sceneHeight == sceneHeight) {
            return true;
        }

        for (Mip& mip : m_mips) {
            mip.texture.Reset();
            mip.width = 0;
            mip.height = 0;
        }
        m_mipCount = 0;

        // Descriptors are allocated once: the shared SRV heap is a bump allocator, so a resize must
        // rewrite the same slots instead of taking new ones.
        if (!m_srvAllocated) {
            if (!allocateSrv) {
                return false;
            }
            for (Mip& mip : m_mips) {
                if (!allocateSrv(1u, mip.srvCpu, mip.srvGpu)) {
                    return false;
                }
            }
            DescriptorHeapDesc rtvHeapDesc{};
            rtvHeapDesc.NumDescriptors = MAX_MIPS;
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(device.CreateDescriptorHeap(rtvHeapDesc, m_rtvHeap))) {
                return false;
            }
            const UINT rtvStride = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            CpuDescriptorHandle rtv = m_rtvHeap.GetCPUDescriptorHandleForHeapStart();
            for (Mip& mip : m_mips) {
                mip.rtv = rtv;
                rtv.ptr += rtvStride;
            }
            m_srvAllocated = true;
        }

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        UINT width = sceneWidth;
        UINT height = sceneHeight;
        UINT count = 0;
        for (; count < MAX_MIPS; ++count) {
            // Stop before a level degenerates; a 1-texel-wide mip only smears the edge.
            if (width < 4u || height < 4u) {
                break;
            }
            width = (std::max)(1u, width / 2u);
            height = (std::max)(1u, height / 2u);

            Mip& mip = m_mips[count];
            const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
                MIP_FORMAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            if (FAILED(device.CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       nullptr, mip.texture))) {
                for (Mip& created : m_mips) {
                    created.texture.Reset();
                }
                return false;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = MIP_FORMAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(mip.texture, &srvDesc, mip.srvCpu);
            device.CreateRenderTargetView(mip.texture, nullptr, mip.rtv);

            mip.width = width;
            mip.height = height;
        }
        if (count == 0) {
            return false;
        }

        m_mipCount = count;
        m_sceneWidth = sceneWidth;
        m_sceneHeight = sceneHeight;
        return true;
    }

    void BloomRenderer::DrawStep(CommandList& cmdList, const Mip& target, GpuDescriptorHandle source,
                                 UINT sourceWidth, UINT sourceHeight, float karis, float radius,
                                 RendererFrameCoordinator& frameCoordinator,
                                 RendererFrameCoordinator::FrameContext& frame) const
    {
        const Viewport viewport{ 0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f };
        const Rect scissor{ 0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height) };
        cmdList.RSSetViewports(1, &viewport);
        cmdList.RSSetScissorRects(1, &scissor);
        cmdList.OMSetRenderTargets(1, &target.rtv, FALSE, nullptr);
        cmdList.SetGraphicsRootDescriptorTable(0, source);

        // Same CB shape as the tone map pass: extra0 lands at offset 128, where BloomCB declares
        // u_bloomParams; the two matrices are padding.
        const float unusedMatrix[16] = {};
        const float params[4] = {
            1.0f / static_cast<float>(sourceWidth),
            1.0f / static_cast<float>(sourceHeight),
            karis,
            radius
        };
        const D3D12_GPU_VIRTUAL_ADDRESS cb =
            frameCoordinator.PushCameraCB(frame, unusedMatrix, unusedMatrix, params, nullptr, nullptr);
        if (cb != 0) {
            cmdList.SetGraphicsRootConstantBufferView(2, cb);
        }
        cmdList.DrawInstanced(3u, 1u, 0u, 0u);
    }

    bool BloomRenderer::Execute(CommandList& cmdList,
                                GpuDescriptorHandle sceneColorSrv,
                                RenderPipelineStateCache& pipelineStateCache,
                                RendererFrameCoordinator& frameCoordinator,
                                RendererFrameCoordinator::FrameContext& frame,
                                float radius) const
    {
        if (m_mipCount == 0) {
            return false;
        }

        auto toTarget = [&](const Mip& mip) {
            const ResourceBarrier barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                mip.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList.ResourceBarrier(1, &barrier);
        };
        auto toShaderResource = [&](const Mip& mip) {
            const ResourceBarrier barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                mip.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList.ResourceBarrier(1, &barrier);
        };

        cmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Down: each mip replaces its whole content, so no clear is needed.
        cmdList.SetPipelineState(pipelineStateCache.GetBloomDownsamplePipelineState());
        GpuDescriptorHandle source = sceneColorSrv;
        UINT sourceWidth = m_sceneWidth;
        UINT sourceHeight = m_sceneHeight;
        for (UINT i = 0; i < m_mipCount; ++i) {
            const Mip& mip = m_mips[i];
            toTarget(mip);
            DrawStep(cmdList, mip, source, sourceWidth, sourceHeight, i == 0 ? 1.0f : 0.0f, 0.0f,
                     frameCoordinator, frame);
            toShaderResource(mip);
            source = mip.srvGpu;
            sourceWidth = mip.width;
            sourceHeight = mip.height;
        }

        // Up: blur mip i and add it onto mip i-1, so mip0 ends up with every level summed.
        cmdList.SetPipelineState(pipelineStateCache.GetBloomUpsamplePipelineState());
        for (UINT i = m_mipCount - 1; i > 0; --i) {
            const Mip& smaller = m_mips[i];
            const Mip& larger = m_mips[i - 1];
            toTarget(larger);
            DrawStep(cmdList, larger, smaller.srvGpu, smaller.width, smaller.height, 0.0f, radius,
                     frameCoordinator, frame);
            toShaderResource(larger);
        }
        return true;
    }
}
