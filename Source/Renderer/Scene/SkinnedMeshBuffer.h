#pragma once
#include <vector>

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RhiDevice.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiRenderer
{
    // GPU-side vertex/index buffers for a list of SkinnedMeshes (68-byte stride)
    class SkinnedMeshBuffer
    {
    public:
        // Reuses MeshBuffer::GPUItem format — VBV stride is sizeof(SkinnedVertex)
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

        bool Upload(GraphicsDevice& device, const std::vector<SkinnedMesh>& meshes);

        const std::vector<GPUItem>& Items() const { return m_items; }

        void Bind(IRhiCommandEncoder* enc, size_t i);

    private:
        std::vector<GPUItem> m_items;
    };
}
