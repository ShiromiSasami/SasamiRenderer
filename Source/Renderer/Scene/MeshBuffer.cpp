#include "Renderer/Scene/MeshBuffer.h"
#include <cstring>
#include <utility>

namespace SasamiRenderer
{
    bool MeshBuffer::Upload(GraphicsDevice& device, const std::vector<Mesh>& meshes)
    {
        m_items.clear();
        m_items.reserve(meshes.size());
        if (meshes.empty()) {
            return false;
        }

        if (!device.GetCapabilities().supportsD3D12CompatibilitySurface &&
            device.GetCapabilities().supportsRhiResourceCreation) {
            for (const auto& m : meshes) {
                GPUItem item{};

                if (!m.vertices.empty()) {
                    const uint64_t vbBytes = static_cast<uint64_t>(sizeof(Vertex)) * m.vertices.size();
                    RhiBufferDesc vbDesc{};
                    vbDesc.sizeInBytes = vbBytes;
                    vbDesc.strideInBytes = sizeof(Vertex);
                    vbDesc.usage = RhiBufferUsageFlags::Vertex;
                    vbDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
                    vbDesc.initialState = RhiResourceState::Common;
                    item.rhiVb = device.CreateRhiBuffer(vbDesc, m.vertices.data());
                    if (!item.rhiVb.IsValid()) {
                        return false;
                    }
                    item.vbv.BufferLocation = item.rhiVb.id;
                    item.vbv.StrideInBytes = sizeof(Vertex);
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
                        return false;
                    }
                    item.ibv.BufferLocation = item.rhiIb.id;
                    item.ibv.SizeInBytes = static_cast<UINT>(ibBytes);
                    item.ibv.Format = DXGI_FORMAT_R32_UINT;
                    item.indexCount = static_cast<UINT>(m.indices.size());
                }

                m_items.push_back(std::move(item));
            }
            return !m_items.empty();
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
            GPUItem item{};

            // Vertex buffer
            if (!m.vertices.empty()) {
                const UINT64 vbBytes = static_cast<UINT64>(sizeof(Vertex)) * m.vertices.size();

                D3D12_HEAP_PROPERTIES heapDefault{}; heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC descVB{}; descVB.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                descVB.Width = vbBytes;
                descVB.Height = 1; descVB.DepthOrArraySize = 1; descVB.MipLevels = 1; descVB.SampleDesc.Count = 1; descVB.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                hr = device.CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &descVB,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, item.vb);
                if (FAILED(hr)) {
                    return false;
                }

                Resource vbUpload;
                D3D12_HEAP_PROPERTIES heapUpload{}; heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
                hr = device.CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &descVB,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, vbUpload);
                if (FAILED(hr)) {
                    return false;
                }

                void* pVB = nullptr; vbUpload->Map(0, nullptr, &pVB);
                memcpy(pVB, m.vertices.data(), sizeof(Vertex) * m.vertices.size());
                vbUpload->Unmap(0, nullptr);

                uploadList.CopyBufferRegion(item.vb, 0, vbUpload, 0, vbBytes);
                auto vbBarrier = Transition(item.vb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                uploadList.ResourceBarrier(1, &vbBarrier);

                item.vbv.BufferLocation = item.vb->GetGPUVirtualAddress();
                item.vbv.StrideInBytes = sizeof(Vertex);
                item.vbv.SizeInBytes = static_cast<UINT>(sizeof(Vertex) * m.vertices.size());
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
                    return false;
                }

                Resource ibUpload;
                D3D12_HEAP_PROPERTIES heapUpload{}; heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
                hr = device.CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &descIB,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, ibUpload);
                if (FAILED(hr)) {
                    return false;
                }

                void* pIB = nullptr; ibUpload->Map(0, nullptr, &pIB);
                memcpy(pIB, m.indices.data(), sizeof(uint32_t) * m.indices.size());
                ibUpload->Unmap(0, nullptr);

                uploadList.CopyBufferRegion(item.ib, 0, ibUpload, 0, ibBytes);
                auto ibBarrier = Transition(item.ib, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
                uploadList.ResourceBarrier(1, &ibBarrier);

                item.ibv.BufferLocation = item.ib->GetGPUVirtualAddress();
                item.ibv.SizeInBytes = static_cast<UINT>(sizeof(uint32_t) * m.indices.size());
                item.ibv.Format = DXGI_FORMAT_R32_UINT;
                item.indexCount = static_cast<UINT>(m.indices.size());
                uploadBuffers.push_back(std::move(ibUpload));
            }

            m_items.push_back(std::move(item));
        }

        hr = uploadList.Close();
        if (FAILED(hr)) {
            return false;
        }
        ID3D12CommandList* lists[] = { uploadList.Get() };
        device.GetCommandQueue()->ExecuteCommandLists(1, lists);
        device.WaitForGPU();

        return !m_items.empty();
    }

    void MeshBuffer::Bind(IRhiCommandEncoder* enc, size_t i)
    {
        if (!enc || i >= m_items.size()) return;
        auto& it = m_items[i];
        if (it.rhiVb.IsValid()) {
            RhiVertexBufferView vbv{ it.rhiVb.id, it.vbv.StrideInBytes, it.vbv.SizeInBytes };
            enc->SetVertexBuffers(0, 1, &vbv);
        } else if (it.vb.IsValid()) {
            RhiVertexBufferView vbv{ it.vbv.BufferLocation, it.vbv.StrideInBytes, it.vbv.SizeInBytes };
            enc->SetVertexBuffers(0, 1, &vbv);
        }
        if (it.rhiIb.IsValid()) {
            RhiIndexBufferView ibv{ it.rhiIb.id, it.ibv.SizeInBytes,
                                    it.ibv.Format == DXGI_FORMAT_R32_UINT };
            enc->SetIndexBuffer(ibv);
        } else if (it.ib.IsValid()) {
            RhiIndexBufferView ibv{ it.ibv.BufferLocation, it.ibv.SizeInBytes,
                                    it.ibv.Format == DXGI_FORMAT_R32_UINT };
            enc->SetIndexBuffer(ibv);
        }
    }
}
