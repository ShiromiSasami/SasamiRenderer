#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
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
        // 64 triangles per meshlet: with no vertex dedup (3 output verts/tri, no
        // sharing) the D3D12 mesh shader hard limit of 256 vertices/256 primitives
        // caps this at floor(256/3) = 84; 64 keeps numthreads(64) at exactly one
        // wave and yields 192 output vertices, comfortably under the 256 cap.
        static constexpr uint32_t kMaxTrianglesPerMeshlet = 64u;

        ~MeshletBuffer();

        // Build meshlets from all submitted meshes.
        void Build(const std::vector<Mesh>& meshes);

        // Upload to GPU. Safe to call every time Build() is called.
        bool Upload(IRHIDevice& device);
        void Release();

        // GPU virtual addresses (valid after Upload)
        D3D12_GPU_VIRTUAL_ADDRESS GetMeshletDescGpuVA()  const;
        D3D12_GPU_VIRTUAL_ADDRESS GetMeshletIndexGpuVA() const;

        uint32_t GetTotalMeshletCount() const { return static_cast<uint32_t>(m_meshletDescs.size()); }

        // Per-mesh meshlet range (for per-draw dispatch)
        struct MeshRange { uint32_t meshletOffset; uint32_t meshletCount; };
        const MeshRange& GetMeshRange(size_t meshIndex) const { return m_meshRanges[meshIndex]; }
        size_t GetMeshRangeCount() const { return m_meshRanges.size(); }

        bool IsValid() const;

    private:
        Resource* GetDescBufferResource() const;
        Resource* GetIndexBufferResource() const;

        std::vector<MeshletDesc> m_meshletDescs;
        std::vector<uint32_t>    m_meshletIndices; // flat: 3 indices per triangle per meshlet
        std::vector<MeshRange>   m_meshRanges;

        Resource m_descBuffer;   // StructuredBuffer<MeshletDesc>
        Resource m_indexBuffer;  // Buffer<uint>
        RhiBufferHandle m_descBufferHandle{};
        RhiBufferHandle m_indexBufferHandle{};
        Resource* m_descBufferCompat = nullptr;
        Resource* m_indexBufferCompat = nullptr;
        IRHIDevice* m_device = nullptr;
    };
}
