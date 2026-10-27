#pragma once
#include <vector>

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Scene/DeferredUploadSink.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::Render
{
    // GPU-side vertex/index buffers for a list of SkinnedMeshes (68-byte stride)
    class SkinnedMeshBuffer
    {
    public:
        // Reuses MeshBuffer::GPUItem format  EVBV stride is sizeof(SkinnedVertex)
        struct GPUItem {
            Resource vb;
            Resource ib;
            RhiBufferHandle rhiVb{};
            RhiBufferHandle rhiIb{};
            VertexBufferView vbv{};
            IndexBufferView  ibv{};
            UINT vertexCount = 0;
            UINT indexCount  = 0;
        };

        ~SkinnedMeshBuffer();

        bool Upload(GraphicsDevice& device, const std::vector<SkinnedMesh>& meshes,
                    const DeferredUploadSink* sink = nullptr);
        void Release();

        const std::vector<GPUItem>& Items() const { return m_items; }

        void Bind(IRhiCommandEncoder* enc, size_t i);

    private:
        GraphicsDevice* m_device = nullptr;
        std::vector<GPUItem> m_items;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
