#include "Renderer/Scene/SkinnedMeshBuffer.h"
#include "Renderer/Scene/MeshBufferUploadUtility.h"
#include "Renderer/Structures/SkinnedVertex.h"

namespace SasamiRenderer
{
    SkinnedMeshBuffer::~SkinnedMeshBuffer()
    {
        Release();
    }

    void SkinnedMeshBuffer::Release()
    {
        MeshBufferUploadUtility::Release(m_device, m_items);
    }

    bool SkinnedMeshBuffer::Upload(GraphicsDevice& device, const std::vector<SkinnedMesh>& meshes,
                                   const DeferredUploadSink* sink)
    {
        return MeshBufferUploadUtility::Upload<SkinnedVertex>(device, meshes, m_device, m_items, sink);
    }

    void SkinnedMeshBuffer::Bind(IRhiCommandEncoder* enc, size_t i)
    {
        MeshBufferUploadUtility::Bind(enc, i, m_items);
    }
}
