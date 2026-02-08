#pragma once
#include <vector>

#include "GraphicsDevice.h"
#include "Mesh.h"

namespace SasamiRenderer
{
    // Owns GPU-side buffers for a list of Meshes
    class MeshBuffer
    {
    public:
        struct GPUItem {
            Resource vb;
            Resource ib;
            VertexBufferView vbv{};
            IndexBufferView  ibv{};
            UINT vertexCount = 0;
            UINT indexCount = 0;
        };

        bool Upload(GraphicsDevice& device, const std::vector<Mesh>& meshes);

        const std::vector<GPUItem>& Items() const { return m_items; }

        void Bind(CommandList* cmdList, size_t i);

    private:
        std::vector<GPUItem> m_items;
    };
}
