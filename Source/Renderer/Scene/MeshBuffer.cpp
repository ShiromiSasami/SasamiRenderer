#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/MeshBufferUploadUtility.h"

namespace SasamiRenderer
{
    MeshBuffer::~MeshBuffer()
    {
        Release();
    }

    void MeshBuffer::Release()
    {
        MeshBufferUploadUtility::Release(m_device, m_items);
    }

    bool MeshBuffer::Upload(GraphicsDevice& device, const std::vector<Mesh>& meshes,
                            const DeferredUploadSink* sink)
    {
        return MeshBufferUploadUtility::Upload<Vertex>(device, meshes, m_device, m_items, sink);
    }

    void MeshBuffer::Bind(IRhiCommandEncoder* enc, size_t i)
    {
        MeshBufferUploadUtility::Bind(enc, i, m_items);
    }
}
