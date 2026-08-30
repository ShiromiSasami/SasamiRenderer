#pragma once

#include <cstring>
#include <utility>
#include <vector>

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Scene/DeferredUploadSink.h"

namespace SasamiRenderer
{
    // Shared GPU vertex/index buffer upload+bind logic for MeshBuffer and SkinnedMeshBuffer.
    // VertexT is the only axis of variation (Vertex vs SkinnedVertex); GPUItemT is each
    // owner's own GPUItem type (structurally identical, kept separate per class).
    namespace MeshBufferUploadUtility
    {
        template<typename GPUItemT>
        void Release(GraphicsDevice*& device, std::vector<GPUItemT>& items)
        {
            if (device) {
                for (auto& item : items) {
                    if (item.rhiVb.IsValid()) {
                        device->DestroyRhiResource(item.rhiVb);
                    }
                    if (item.rhiIb.IsValid()) {
                        device->DestroyRhiResource(item.rhiIb);
                    }
                }
            }
            items.clear();
        }

        template<typename VertexT, typename MeshT, typename GPUItemT>
        bool Upload(GraphicsDevice& device, const std::vector<MeshT>& meshes,
                    GraphicsDevice*& deviceMember, std::vector<GPUItemT>& items,
                    const DeferredUploadSink* sink = nullptr)
        {
            // The previous upload may still be executing when a sink deferred its
            // retirement instead of blocking, and the buffers it wrote are also still
            // bound by in-flight draws. Release() destroys them unconditionally, so drain
            // the queue first -- destroying a resource the GPU is reading takes the device
            // down (seen with Bistro: ~1600 submeshes replaced in one submit).
            if (!items.empty()) {
                device.WaitForGPU();
            }
            Release(deviceMember, items);
            deviceMember = &device;
            items.reserve(meshes.size());
            if (meshes.empty()) {
                return false;
            }

            if (device.GetCapabilities().supportsRhiResourceCreation) {
                for (const auto& m : meshes) {
                    GPUItemT item{};

                    if (!m.vertices.empty()) {
                        const uint64_t vbBytes = static_cast<uint64_t>(sizeof(VertexT)) * m.vertices.size();
                        RhiBufferDesc vbDesc{};
                        vbDesc.sizeInBytes = vbBytes;
                        vbDesc.strideInBytes = sizeof(VertexT);
                        vbDesc.usage = RhiBufferUsageFlags::Vertex;
                        vbDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
                        vbDesc.initialState = RhiResourceState::Common;
                        item.rhiVb = device.CreateRhiBuffer(vbDesc, m.vertices.data());
                        if (!item.rhiVb.IsValid()) {
                            Release(deviceMember, items);
                            return false;
                        }
                        item.vbv.StrideInBytes = sizeof(VertexT);
                        item.vbv.SizeInBytes = static_cast<UINT>(vbBytes);
                        item.vertexCount = static_cast<UINT>(m.vertices.size());
                    }

                    if (!m.indices.empty()) {
                        const uint64_t ibBytes = static_cast<uint64_t>(sizeof(uint32_t)) * m.indices.size();
                        RhiBufferDesc ibDesc{};
                        ibDesc.sizeInBytes = ibBytes;
                        ibDesc.strideInBytes = sizeof(uint32_t);
                        ibDesc.usage = RhiBufferUsageFlags::Index;
                        ibDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
                        ibDesc.initialState = RhiResourceState::Common;
                        item.rhiIb = device.CreateRhiBuffer(ibDesc, m.indices.data());
                        if (!item.rhiIb.IsValid()) {
                            if (item.rhiVb.IsValid()) {
                                device.DestroyRhiResource(item.rhiVb);
                            }
                            Release(deviceMember, items);
                            return false;
                        }
                        item.ibv.SizeInBytes = static_cast<UINT>(ibBytes);
                        item.ibv.Format = DXGI_FORMAT_R32_UINT;
                        item.indexCount = static_cast<UINT>(m.indices.size());
                    }

                    items.push_back(std::move(item));
                }
                return !items.empty();
            }

            if (!device.GetCapabilities().supportsD3D12CompatibilitySurface) {
                return false;
            }

            CommandAllocator uploadAlloc;
            CommandList uploadList;
            HRESULT hr = device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc);
            if (FAILED(hr)) {
                return false;
            }
            hr = device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc, nullptr, uploadList);
            if (FAILED(hr)) {
                return false;
            }

            std::vector<Resource> uploadBuffers;
            uploadBuffers.reserve(meshes.size() * 2);

            for (const auto& m : meshes) {
                GPUItemT item{};

                // Vertex buffer
                if (!m.vertices.empty()) {
                    const UINT64 vbBytes = static_cast<UINT64>(sizeof(VertexT)) * m.vertices.size();

                    D3D12_HEAP_PROPERTIES heapDefault{}; heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC descVB{}; descVB.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    descVB.Width = vbBytes;
                    descVB.Height = 1; descVB.DepthOrArraySize = 1; descVB.MipLevels = 1; descVB.SampleDesc.Count = 1; descVB.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    hr = device.CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &descVB,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, item.vb);
                    if (FAILED(hr)) {
                        Release(deviceMember, items);
                        return false;
                    }

                    Resource vbUpload;
                    D3D12_HEAP_PROPERTIES heapUpload{}; heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
                    hr = device.CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &descVB,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, vbUpload);
                    if (FAILED(hr)) {
                        Release(deviceMember, items);
                        return false;
                    }

                    void* pVB = nullptr;
                    hr = vbUpload->Map(0, nullptr, &pVB);
                    if (FAILED(hr) || !pVB) {
                        Release(deviceMember, items);
                        return false;
                    }
                    std::memcpy(pVB, m.vertices.data(), sizeof(VertexT) * m.vertices.size());
                    vbUpload->Unmap(0, nullptr);

                    uploadList.CopyBufferRegion(item.vb, 0, vbUpload, 0, vbBytes);
                    auto vbBarrier = Transition(item.vb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                    uploadList.ResourceBarrier(1, &vbBarrier);

                    item.vbv.BufferLocation = item.vb->GetGPUVirtualAddress();
                    item.vbv.StrideInBytes = sizeof(VertexT);
                    item.vbv.SizeInBytes = static_cast<UINT>(vbBytes);
                    item.vertexCount = static_cast<UINT>(m.vertices.size());
                    uploadBuffers.push_back(std::move(vbUpload));
                }

                // Index buffer (if present)
                if (!m.indices.empty()) {
                    const UINT64 ibBytes = static_cast<UINT64>(sizeof(uint32_t)) * m.indices.size();

                    D3D12_HEAP_PROPERTIES heapDefault{}; heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC descIB{}; descIB.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    descIB.Width = ibBytes;
                    descIB.Height = 1; descIB.DepthOrArraySize = 1; descIB.MipLevels = 1; descIB.SampleDesc.Count = 1; descIB.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    hr = device.CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &descIB,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, item.ib);
                    if (FAILED(hr)) {
                        Release(deviceMember, items);
                        return false;
                    }

                    Resource ibUpload;
                    D3D12_HEAP_PROPERTIES heapUpload{}; heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
                    hr = device.CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &descIB,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, ibUpload);
                    if (FAILED(hr)) {
                        Release(deviceMember, items);
                        return false;
                    }

                    void* pIB = nullptr;
                    hr = ibUpload->Map(0, nullptr, &pIB);
                    if (FAILED(hr) || !pIB) {
                        Release(deviceMember, items);
                        return false;
                    }
                    std::memcpy(pIB, m.indices.data(), sizeof(uint32_t) * m.indices.size());
                    ibUpload->Unmap(0, nullptr);

                    uploadList.CopyBufferRegion(item.ib, 0, ibUpload, 0, ibBytes);
                    auto ibBarrier = Transition(item.ib, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
                    uploadList.ResourceBarrier(1, &ibBarrier);

                    item.ibv.BufferLocation = item.ib->GetGPUVirtualAddress();
                    item.ibv.SizeInBytes = static_cast<UINT>(ibBytes);
                    item.ibv.Format = DXGI_FORMAT_R32_UINT;
                    item.indexCount = static_cast<UINT>(m.indices.size());
                    uploadBuffers.push_back(std::move(ibUpload));
                }

                items.push_back(std::move(item));
            }

            hr = uploadList.Close();
            if (FAILED(hr)) {
                Release(deviceMember, items);
                return false;
            }
            ID3D12CommandList* lists[] = { uploadList.Get() };
            device.GetCommandQueue()->ExecuteCommandLists(1, lists);
            if (sink && *sink) {
                (*sink)(std::move(uploadAlloc), std::move(uploadList), std::move(uploadBuffers));
            } else {
                device.WaitForGPU();
            }

            return !items.empty();
        }

        template<typename GPUItemT>
        void Bind(IRhiCommandEncoder* enc, size_t i, const std::vector<GPUItemT>& items)
        {
            if (!enc || i >= items.size()) return;
            auto& it = items[i];
            if (it.rhiVb.IsValid()) {
                RhiVertexBufferBinding vb{};
                vb.buffer = it.rhiVb;
                vb.strideInBytes = it.vbv.StrideInBytes;
                vb.sizeInBytes = it.vbv.SizeInBytes;
                enc->SetVertexBufferBindings(0, 1, &vb);
            } else if (it.vb.IsValid()) {
                RhiVertexBufferView vbv{ it.vbv.BufferLocation, it.vbv.StrideInBytes, it.vbv.SizeInBytes };
                enc->SetVertexBuffers(0, 1, &vbv);
            }
            if (it.rhiIb.IsValid()) {
                RhiIndexBufferBinding ib{};
                ib.buffer = it.rhiIb;
                ib.sizeInBytes = it.ibv.SizeInBytes;
                ib.is32Bit = it.ibv.Format == DXGI_FORMAT_R32_UINT;
                enc->SetIndexBufferBinding(ib);
            } else if (it.ib.IsValid()) {
                RhiIndexBufferView ibv{ it.ibv.BufferLocation, it.ibv.SizeInBytes,
                                        it.ibv.Format == DXGI_FORMAT_R32_UINT };
                enc->SetIndexBuffer(ibv);
            }
        }
    }
}
