#pragma once
#include <vector>

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiRenderer
{
    // Owns GPU-side buffers for a list of Meshes
    class MeshBuffer
    {
    public:
        struct GPUItem {
            Resource vb;
            Resource ib;
            RhiBufferHandle rhiVb{};
            RhiBufferHandle rhiIb{};
            VertexBufferView vbv{};
            IndexBufferView  ibv{};
            UINT vertexCount = 0;
            UINT indexCount = 0;
        };

        ~MeshBuffer();

        bool Upload(GraphicsDevice& device, const std::vector<Mesh>& meshes);
        void Release();

        const std::vector<GPUItem>& Items() const { return m_items; }

        void Bind(IRhiCommandEncoder* enc, size_t i);

    private:
        GraphicsDevice* m_device = nullptr;
        std::vector<GPUItem> m_items;
    };
}
