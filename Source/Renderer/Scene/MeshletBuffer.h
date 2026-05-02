#pragma once
#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Structures/Mesh.h"
#include <cstdint>
#include <vector>

namespace SasamiRenderer
{
    // CPU-side meshlet descriptor (matches HLSL MeshletDesc)
    struct MeshletDesc
    {
        uint32_t indexOffset;     // First index into the flat meshlet index buffer (3 indices per tri)
        uint32_t indexCount;      // Number of triangles in this meshlet
        float    boundsCenter[3];
        float    boundsRadius;
    };

    // Manages per-mesh meshlet data:
    //   - Meshlet descriptors (MeshletDesc[])
    //   - Meshlet index buffer (uint32 flat: 3 indices per triangle)
    // One MeshletBuffer per scene; rebuilt when geometry changes.
    class MeshletBuffer
    {
    public:
        // 16 triangles per meshlet: allows LOD-0 subdivision (×4) → 64 output tris,
        // fitting within the MS 256-primitive / 256-vertex hard limits.
        static constexpr uint32_t kMaxTrianglesPerMeshlet = 16u;

        // Build meshlets from all submitted meshes.
        void Build(const std::vector<Mesh>& meshes);

        // Upload to GPU. Safe to call every time Build() is called.
        bool Upload(IRHIDevice& device);

        // GPU virtual addresses (valid after Upload)
        D3D12_GPU_VIRTUAL_ADDRESS GetMeshletDescGpuVA()  const;
        D3D12_GPU_VIRTUAL_ADDRESS GetMeshletIndexGpuVA() const;

        uint32_t GetTotalMeshletCount() const { return static_cast<uint32_t>(m_meshletDescs.size()); }

        // Per-mesh meshlet range (for per-draw dispatch)
        struct MeshRange { uint32_t meshletOffset; uint32_t meshletCount; };
        const MeshRange& GetMeshRange(size_t meshIndex) const { return m_meshRanges[meshIndex]; }
        size_t GetMeshRangeCount() const { return m_meshRanges.size(); }

        bool IsValid() const { return m_descBuffer.IsValid() && m_indexBuffer.IsValid(); }

    private:
        std::vector<MeshletDesc> m_meshletDescs;
        std::vector<uint32_t>    m_meshletIndices; // flat: 3 indices per triangle per meshlet
        std::vector<MeshRange>   m_meshRanges;

        Resource m_descBuffer;   // StructuredBuffer<MeshletDesc>
        Resource m_indexBuffer;  // Buffer<uint>
    };
}
