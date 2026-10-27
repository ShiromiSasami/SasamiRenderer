#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/MeshBufferUploadUtility.h"

namespace SasamiEngine::Render
{
    MeshBuffer::~MeshBuffer()
    {
        Release();
    }

    void MeshBuffer::Release()
    {
        ReleaseDynamicVertices();
        MeshBufferUploadUtility::Release(m_device, m_items);
    }

    void MeshBuffer::ReleaseDynamicVertices()
    {
        for (auto& [index, dynamic] : m_dynamicVertices) {
            if (index < m_items.size()) {
                m_items[index].rhiVb = dynamic.uploadedVb;
            }
            if (m_device) {
                dynamic.ring.Release(*m_device);
            }
        }
        m_dynamicVertices.clear();
    }

    bool MeshBuffer::Upload(GraphicsDevice& device, const std::vector<Mesh>& meshes,
                            const DeferredUploadSink* sink)
    {
        // The utility releases the items itself, which would destroy a ring copy in place of the uploaded
        // buffer. Drain first for the same reason it does: in-flight draws may still read the copies.
        if (!m_dynamicVertices.empty()) {
            device.WaitForGPU();
            ReleaseDynamicVertices();
        }
        return MeshBufferUploadUtility::Upload<Vertex>(device, meshes, m_device, m_items, sink);
    }

    D3D12_GPU_VIRTUAL_ADDRESS MeshBuffer::GetVertexBufferGpuVA(size_t i) const
    {
        if (i >= m_items.size()) {
            return 0;
        }
        const GPUItem& item = m_items[i];
        if (m_device && item.rhiVb.IsValid()) {
            if (const Resource* resource = m_device->GetD3D12CompatibilityResource(item.rhiVb);
                resource && resource->IsValid()) {
                return resource->GetGPUVirtualAddress();
            }
        }
        return item.vbv.BufferLocation;
    }

    bool MeshBuffer::SetDrawIndexCount(size_t i, UINT count)
    {
        if (i >= m_items.size()) {
            return false;
        }
        GPUItem& item = m_items[i];
        // ibv.SizeInBytes keeps the uploaded length (R32 indices) after indexCount was lowered.
        const UINT uploaded = item.ibv.SizeInBytes / sizeof(uint32_t);
        if (count == 0 || count > uploaded) {
            return false;
        }
        item.indexCount = count;
        return true;
    }

    bool MeshBuffer::WriteDynamicVertices(size_t i, const Vertex* vertices, size_t vertexCount, size_t first,
                                          size_t count, uint32_t slotCount, uint64_t frame, size_t* writtenVertices)
    {
        if (!m_device || !vertices || i >= m_items.size()) {
            return false;
        }
        GPUItem& item = m_items[i];
        auto it = m_dynamicVertices.find(i);
        if (it == m_dynamicVertices.end()) {
            if (!item.rhiVb.IsValid() || item.vertexCount != vertexCount) {
                return false;
            }
            DynamicVertices dynamic;
            dynamic.uploadedVb = item.rhiVb;
            if (!dynamic.ring.Create(*m_device, vertices, vertexCount, slotCount)) {
                return false;
            }
            it = m_dynamicVertices.emplace(i, std::move(dynamic)).first;
        }
        if (!it->second.ring.Write(*m_device, vertices, vertexCount, first, count, frame)) {
            return false;
        }
        item.rhiVb = it->second.ring.Current();
        if (writtenVertices) {
            *writtenVertices = it->second.ring.LastWrittenVertices();
        }
        return true;
    }

    void MeshBuffer::Bind(IRhiCommandEncoder* enc, size_t i)
    {
        MeshBufferUploadUtility::Bind(enc, i, m_items);
    }
}
