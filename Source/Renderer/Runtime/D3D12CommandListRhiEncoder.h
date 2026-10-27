#pragma once
// D3D12CommandListRhiEncoder: the DX12 adapter that lets Renderer::Render's render-pass
// graph talk to a raw CommandList through the backend-agnostic IRhiCommandEncoder
// interface. Originally a file-local (anonymous-namespace) type inside
// RendererFrameGraph.cpp, it is shared as source text here because Render()'s frame
// graph construction was split across stage functions living in different translation
// units (RendererDrawCallbacks.cpp, RendererPassServices.cpp), and both need to
// construct instances of it. It has external linkage (NOT an anonymous namespace): a
// including translation unit still gets its own private copy with internal linkage,
// exactly as before -- no ODR risk, no new external API surface.

#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

#include <vector>
#include <cstdint>

namespace SasamiEngine::Render
{
    inline D3D12_RESOURCE_STATES ToCompatibilityDx12State(RhiResourceState state)
    {
        switch (state) {
        case RhiResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case RhiResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case RhiResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
        case RhiResourceState::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case RhiResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case RhiResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case RhiResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
        case RhiResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
        case RhiResourceState::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case RhiResourceState::Common:
        default: return D3D12_RESOURCE_STATE_COMMON;
        }
    }

    inline D3D_PRIMITIVE_TOPOLOGY ToCompatibilityDx12Topology(RhiPrimitiveTopology topology)
    {
        switch (topology) {
        case RhiPrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case RhiPrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case RhiPrimitiveTopology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case RhiPrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case RhiPrimitiveTopology::PatchList: return D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        case RhiPrimitiveTopology::TriangleList:
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }

    class D3D12CommandListRhiEncoder final : public IRhiCommandEncoder
    {
    public:
        D3D12CommandListRhiEncoder(IRHIDevice& device, CommandList& commandList)
            : m_device(device)
            , m_commandList(commandList)
        {
        }

        void UavBarrier(const RhiResourceHandle* resources, uint32_t count) override
        {
            if (!resources || count == 0) {
                const ResourceBarrier barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
                m_commandList.ResourceBarrier(1, &barrier);
                return;
            }

            std::vector<ResourceBarrier> barriers;
            barriers.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Resource* resource = m_device.GetD3D12CompatibilityResource(resources[i]);
                if (!resource || !resource->IsValid()) {
                    continue;
                }
                barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(resource->Get()));
            }
            if (!barriers.empty()) {
                m_commandList.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            }
        }

        void TransitionResources(const RhiResourceTransitionDesc* transitions, uint32_t count) override
        {
            if (!transitions || count == 0) {
                return;
            }

            std::vector<ResourceBarrier> barriers;
            barriers.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Resource* resource = m_device.GetD3D12CompatibilityResource(transitions[i].resource);
                if (!resource || !resource->IsValid()) {
                    continue;
                }

                barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                    resource->Get(),
                    ToCompatibilityDx12State(transitions[i].before),
                    ToCompatibilityDx12State(transitions[i].after),
                    transitions[i].subresource));
            }

            if (!barriers.empty()) {
                m_commandList.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            }
        }

        void SetViewports(const RhiViewport* viewports, uint32_t count) override
        {
            if (!viewports || count == 0) {
                return;
            }

            std::vector<Viewport> dxViewports(count);
            for (uint32_t i = 0; i < count; ++i) {
                dxViewports[i] = {
                    viewports[i].x,
                    viewports[i].y,
                    viewports[i].width,
                    viewports[i].height,
                    viewports[i].minDepth,
                    viewports[i].maxDepth,
                };
            }
            m_commandList.RSSetViewports(count, dxViewports.data());
        }

        void SetScissors(const RhiRect* scissors, uint32_t count) override
        {
            if (!scissors || count == 0) {
                return;
            }

            std::vector<Rect> dxRects(count);
            for (uint32_t i = 0; i < count; ++i) {
                dxRects[i] = { scissors[i].left, scissors[i].top, scissors[i].right, scissors[i].bottom };
            }
            m_commandList.RSSetScissorRects(count, dxRects.data());
        }

        void SetGraphicsPipeline(RhiPipelineHandle handle) override
        {
            if (!handle.IsValid()) return;
            auto* pso = reinterpret_cast<ID3D12PipelineState*>(static_cast<uintptr_t>(handle.id));
            m_commandList.Get()->SetPipelineState(pso);
        }

        void SetComputePipeline(RhiPipelineHandle handle) override
        {
            SetGraphicsPipeline(handle);
        }

        void SetPrimitiveTopology(RhiPrimitiveTopology topology) override
        {
            m_commandList.IASetPrimitiveTopology(ToCompatibilityDx12Topology(topology));
        }

        void Draw(const RhiDrawDesc& desc) override
        {
#if defined(_DEBUG)
            DebugIncrementDrawCount();
#endif
            m_commandList.DrawInstanced(desc.vertexCount,
                                        desc.instanceCount,
                                        desc.startVertex,
                                        desc.startInstance);
        }

        void DrawIndexed(const RhiDrawIndexedDesc& desc) override
        {
#if defined(_DEBUG)
            DebugIncrementDrawCount();
#endif
            m_commandList.DrawIndexedInstanced(desc.indexCount,
                                               desc.instanceCount,
                                               desc.startIndex,
                                               desc.baseVertex,
                                               desc.startInstance);
        }

        void DrawIndexedIndirect(const RhiDrawIndexedIndirectDesc& desc) override
        {
            ID3D12CommandSignature* signature = m_device.GetDx12DrawIndexedCommandSignature();
            Resource* args = m_device.GetD3D12CompatibilityResource(desc.argsBuffer);
            if (!signature || !args || !args->IsValid() || desc.maxDrawCount == 0) {
                return;
            }
            Resource* count = desc.countBuffer.IsValid() ? m_device.GetD3D12CompatibilityResource(desc.countBuffer) : nullptr;
#if defined(_DEBUG)
            DebugIncrementDrawCount();
#endif
            m_commandList.Get()->ExecuteIndirect(signature,
                                                 desc.maxDrawCount,
                                                 args->Get(),
                                                 desc.argsOffsetInBytes,
                                                 (count && count->IsValid()) ? count->Get() : nullptr,
                                                 desc.countOffsetInBytes);
        }

        void Dispatch(const RhiDispatchDesc& desc) override
        {
            m_commandList.Dispatch(desc.groupCountX, desc.groupCountY, desc.groupCountZ);
        }

        // Same bounds checks as the native DX12 encoder; out-of-range copies are skipped.
        void CopyBuffer(const RhiBufferCopyDesc& copy) override
        {
            if (copy.sizeInBytes == 0) {
                return;
            }
            Resource* source = m_device.GetD3D12CompatibilityResource(copy.source);
            Resource* destination = m_device.GetD3D12CompatibilityResource(copy.destination);
            if (!source || !source->IsValid() || !destination || !destination->IsValid()) {
                return;
            }
            const auto sourceDesc = source->Get()->GetDesc();
            const auto destinationDesc = destination->Get()->GetDesc();
            if (copy.sourceOffsetInBytes >= sourceDesc.Width ||
                copy.sizeInBytes > sourceDesc.Width - copy.sourceOffsetInBytes ||
                copy.destinationOffsetInBytes >= destinationDesc.Width ||
                copy.sizeInBytes > destinationDesc.Width - copy.destinationOffsetInBytes) {
                return;
            }
            m_commandList.CopyBufferRegion(*destination, copy.destinationOffsetInBytes, *source,
                                           copy.sourceOffsetInBytes, copy.sizeInBytes);
        }

        void SetGraphicsPipelineLayout(RhiPipelineLayoutHandle handle) override
        {
            if (!handle.IsValid()) return;
            auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(handle.id));
            m_commandList.Get()->SetGraphicsRootSignature(sig);
        }

        void SetComputePipelineLayout(RhiPipelineLayoutHandle handle) override
        {
            if (!handle.IsValid()) return;
            auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(handle.id));
            m_commandList.Get()->SetComputeRootSignature(sig);
        }

        void SetDescriptorHeap(RhiDescriptorHeapHandle handle) override
        {
            if (!handle.IsValid()) return;
            auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(static_cast<uintptr_t>(handle.id));
            m_commandList.SetDescriptorHeaps(1, &heap);
        }

        void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            m_commandList.SetGraphicsRootDescriptorTable(slot, { table.ptr });
        }

        void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            m_commandList.SetComputeRootDescriptorTable(slot, { table.ptr });
        }

        void SetGraphicsConstantBufferView(uint32_t slot, RhiGpuAddress address) override
        {
            m_commandList.SetGraphicsRootConstantBufferView(slot, address);
        }

        void SetComputeConstantBufferView(uint32_t slot, RhiGpuAddress address) override
        {
            m_commandList.SetComputeRootConstantBufferView(slot, address);
        }

        void SetGraphicsShaderResourceView(uint32_t slot, RhiGpuAddress address) override
        {
            m_commandList.SetGraphicsRootShaderResourceView(slot, address);
        }

        void SetComputeShaderResourceView(uint32_t slot, RhiGpuAddress address) override
        {
            m_commandList.SetComputeRootShaderResourceView(slot, address);
        }

        void SetComputeUnorderedAccessView(uint32_t slot, RhiGpuAddress address) override
        {
            m_commandList.Get()->SetComputeRootUnorderedAccessView(slot, address);
        }

        void SetRenderTargets(uint32_t numRtvs,
                              const RhiCpuDescriptorHandle* rtvs,
                              const RhiCpuDescriptorHandle* dsv = nullptr) override
        {
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dxRtvs(numRtvs);
            for (uint32_t i = 0; i < numRtvs; ++i)
                dxRtvs[i] = { static_cast<SIZE_T>(rtvs[i].ptr) };
            const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
            D3D12_CPU_DESCRIPTOR_HANDLE dxDsv{};
            if (dsv && dsv->IsValid()) {
                dxDsv = { static_cast<SIZE_T>(dsv->ptr) };
                dsvPtr = &dxDsv;
            }
            m_commandList.OMSetRenderTargets(numRtvs,
                                              numRtvs > 0 ? dxRtvs.data() : nullptr,
                                              FALSE,
                                              dsvPtr);
        }

        void ClearRenderTarget(RhiCpuDescriptorHandle rtv, const RhiClearColor& color) override
        {
            const float rgba[4] = { color.r, color.g, color.b, color.a };
            m_commandList.ClearRenderTargetView({ static_cast<SIZE_T>(rtv.ptr) }, rgba, 0, nullptr);
        }

        void ClearDepthStencil(RhiCpuDescriptorHandle dsv, float depth, uint8_t stencil) override
        {
            m_commandList.ClearDepthStencilView({ static_cast<SIZE_T>(dsv.ptr) },
                                                D3D12_CLEAR_FLAG_DEPTH,
                                                depth,
                                                stencil,
                                                0, nullptr);
        }

        void SetVertexBuffers(uint32_t startSlot, uint32_t count,
                              const RhiVertexBufferView* views) override
        {
            std::vector<D3D12_VERTEX_BUFFER_VIEW> dxViews(count);
            for (uint32_t i = 0; i < count; ++i) {
                dxViews[i].BufferLocation = views[i].gpuAddress;
                dxViews[i].StrideInBytes  = views[i].strideInBytes;
                dxViews[i].SizeInBytes    = views[i].sizeInBytes;
            }
            m_commandList.IASetVertexBuffers(startSlot, count, dxViews.data());
        }

        void SetIndexBuffer(const RhiIndexBufferView& view) override
        {
            D3D12_INDEX_BUFFER_VIEW ibv{};
            ibv.BufferLocation = view.gpuAddress;
            ibv.SizeInBytes    = view.sizeInBytes;
            ibv.Format         = view.is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            m_commandList.IASetIndexBuffer(&ibv);
        }

        void SetVertexBufferBindings(uint32_t startSlot, uint32_t count,
                                     const RhiVertexBufferBinding* bindings) override
        {
            if (count == 0 || !bindings) return;
            std::vector<D3D12_VERTEX_BUFFER_VIEW> dxViews(count);
            for (uint32_t i = 0; i < count; ++i) {
                Resource* resource = m_device.GetD3D12CompatibilityResource(bindings[i].buffer);
                if (!resource) {
                    continue;
                }
                dxViews[i].BufferLocation = resource->GetGPUVirtualAddress() + bindings[i].offsetInBytes;
                dxViews[i].StrideInBytes  = bindings[i].strideInBytes;
                dxViews[i].SizeInBytes    = bindings[i].sizeInBytes;
            }
            m_commandList.IASetVertexBuffers(startSlot, count, dxViews.data());
        }

        void SetIndexBufferBinding(const RhiIndexBufferBinding& binding) override
        {
            Resource* resource = m_device.GetD3D12CompatibilityResource(binding.buffer);
            if (!resource) {
                return;
            }
            D3D12_INDEX_BUFFER_VIEW ibv{};
            ibv.BufferLocation = resource->GetGPUVirtualAddress() + binding.offsetInBytes;
            ibv.SizeInBytes    = binding.sizeInBytes;
            ibv.Format         = binding.is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            m_commandList.IASetIndexBuffer(&ibv);
        }

    private:
        IRHIDevice& m_device;
        CommandList& m_commandList;
    };
}
