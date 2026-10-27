#pragma once
#include <unordered_map>
#include <vector>

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Scene/DeferredUploadSink.h"
#include "Renderer/Scene/DynamicVertexRing.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::Render
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

        bool Upload(GraphicsDevice& device, const std::vector<Mesh>& meshes,
                    const DeferredUploadSink* sink = nullptr);
        void Release();

        const std::vector<GPUItem>& Items() const { return m_items; }

        void Bind(IRhiCommandEncoder* enc, size_t i);

        // GPU virtual address of mesh i's vertex buffer for root-descriptor SRV binding (the
        // virtual-geometry mesh shader reads vertices as a StructuredBuffer). Resolves the RHI
        // handle first and falls back to the legacy view; 0 when the mesh has no vertex buffer.
        D3D12_GPU_VIRTUAL_ADDRESS GetVertexBufferGpuVA(size_t i) const;

        // Draws only the first `count` indices of mesh i from now on (every vertex-path draw reads
        // GPUItem::indexCount). For a fixed-capacity mesh whose tail is unused; the next Upload restores the
        // full count. False when mesh i has no index buffer or `count` is 0 or past the uploaded indices.
        bool SetDrawIndexCount(size_t i, UINT count);

        // Writes vertices [first, first + count) of mesh i through a DynamicVertexRing of `slotCount` copies and
        // points the item's rhiVb at the copy just written, so every draw path follows it. The ring is created
        // on the first call from `vertices`, the full array (vertexCount must equal the uploaded count), which
        // must hold all of the mesh's current vertices. `frame` identifies the rendered frame (see
        // DynamicVertexRing::Write). The uploaded buffer stays allocated but unused until the next Upload / Release.
        bool WriteDynamicVertices(size_t i, const Vertex* vertices, size_t vertexCount, size_t first, size_t count,
                                  uint32_t slotCount, uint64_t frame, size_t* writtenVertices = nullptr);
        // True once WriteDynamicVertices has given mesh i a ring: its rhiVb then changes from frame to frame.
        bool HasDynamicVertices(size_t i) const { return m_dynamicVertices.count(i) != 0; }

    private:
        struct DynamicVertices
        {
            RhiBufferHandle uploadedVb{};   // the item's own buffer, restored before Release destroys it
            DynamicVertexRing ring;
        };

        void ReleaseDynamicVertices();

        GraphicsDevice* m_device = nullptr;
        std::vector<GPUItem> m_items;
        std::unordered_map<size_t, DynamicVertices> m_dynamicVertices;   // keyed by item index
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
