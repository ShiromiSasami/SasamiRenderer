#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/VirtualGeometry/ClusterDag.h"
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    struct Mesh;
    class ClusterDagStore;

    // Matches HLSL ClusterDesc (48 bytes)
    struct GpuClusterDesc
    {
        uint32_t vertexOffset;    // into the cluster-vertex buffer
        uint32_t vertexCount;     // <= 128
        uint32_t triangleOffset;  // into the cluster-triangle buffer
        uint32_t triangleCount;   // <= 128
        uint32_t group;           // ABSOLUTE index into the group buffer
        uint32_t refinedGroup;    // ABSOLUTE index or ClusterDag::kInvalidIndex
        float    boundsCenter[3];
        float    boundsRadius;
        float    pad[2];
    };
    static_assert(sizeof(GpuClusterDesc) == 48);

    // Matches HLSL GroupDesc (32 bytes)
    struct GpuGroupDesc
    {
        float    center[3];
        float    radius;
        float    error;
        uint32_t depth;
        uint32_t firstCluster;    // ABSOLUTE first cluster index into the scene-wide cluster array
        uint32_t clusterCount;    // member count
    };
    static_assert(sizeof(GpuGroupDesc) == 32);

    // Matches HLSL NodeDesc (32 bytes)
    struct GpuNodeDesc
    {
        float    center[3];
        float    radius;
        float    error;
        uint32_t group;       // ABSOLUTE group index or ClusterDag::kInvalidIndex
        uint32_t childOffset; // ABSOLUTE node index
        uint32_t childCount;
    };
    static_assert(sizeof(GpuNodeDesc) == 32);

    // Scene-wide GPU cluster data for the virtual-geometry mesh-shader path: cluster/group
    // descriptors, per-cluster unique vertex ids (into each mesh's own vertex buffer) and
    // 8:8:8-packed local triangles. One instance per scene, rebuilt when geometry changes.
    class ClusterDagGpuBuffer
    {
    public:
        static constexpr uint32_t kMaxVerticesPerCluster  = 128u;
        static constexpr uint32_t kMaxTrianglesPerCluster = 128u;

        struct MeshRange
        {
            uint32_t clusterOffset = 0;
            uint32_t clusterCount  = 0;
            uint32_t groupOffset   = 0;
            uint32_t groupCount    = 0;
            uint32_t nodeOffset    = 0;
            uint32_t nodeCount     = 0;
            uint32_t rootNodeCount = 0;
        };

        ~ClusterDagGpuBuffer();

        // CPU pack. meshes[i] pairs with dagStore.GetDag(i) (nullptr/empty -> empty range).
        void Build(const std::vector<Mesh>& meshes, const ClusterDagStore& dagStore);

        // Upload to GPU. Requires GetCapabilities().supportsRhiResourceCreation; no legacy fallback.
        bool Upload(IRHIDevice& device);
        void Release();

        // All five buffers uploaded.
        bool IsValid() const;

        D3D12_GPU_VIRTUAL_ADDRESS GetClusterDescGpuVA()     const;
        D3D12_GPU_VIRTUAL_ADDRESS GetGroupDescGpuVA()       const;
        D3D12_GPU_VIRTUAL_ADDRESS GetClusterVertexGpuVA()   const;
        D3D12_GPU_VIRTUAL_ADDRESS GetClusterTriangleGpuVA() const;
        D3D12_GPU_VIRTUAL_ADDRESS GetNodeDescGpuVA()        const;

        const MeshRange& GetMeshRange(size_t meshIndex) const { return m_meshRanges[meshIndex]; }
        size_t   GetMeshRangeCount()      const { return m_meshRanges.size(); }
        uint32_t GetTotalClusterCount()   const { return static_cast<uint32_t>(m_clusters.size()); }
        uint32_t GetTotalGroupCount()     const { return static_cast<uint32_t>(m_groups.size()); }
        // CPU copy of the uploaded group descriptors (residency pinning reads the terminal-group errors).
        const std::vector<GpuGroupDesc>& GetGroups() const { return m_groups; }
        // Bitset (32 groups per word) of the terminal/root groups: the coarsest level of every
        // mesh, marked by a sentinel error of FLT_MAX so that no LOD threshold can ever refine
        // past them. Residency pins exactly these, because evicting one leaves a mesh with no
        // drawable level at all; the sentinel itself stays knowledge of the DAG, which writes it.
        std::vector<uint32_t> BuildTerminalGroupBits() const;
        const std::vector<GpuClusterDesc>& GetClusters() const { return m_clusters; }
        const std::vector<uint32_t>& GetClusterVertices() const { return m_clusterVertices; }
        const std::vector<uint32_t>& GetClusterTriangles() const { return m_clusterTriangles; }
        uint32_t GetTotalNodeCount()      const { return static_cast<uint32_t>(m_nodes.size()); }
        uint32_t GetSkippedClusterCount() const { return m_skippedClusters; }

    private:
        Resource* GetClusterDescBufferResource()     const;
        Resource* GetGroupDescBufferResource()        const;
        Resource* GetClusterVertexBufferResource()    const;
        Resource* GetClusterTriangleBufferResource()  const;
        Resource* GetNodeDescBufferResource()         const;

        std::vector<GpuClusterDesc> m_clusters;
        std::vector<GpuGroupDesc>   m_groups;
        std::vector<uint32_t>       m_clusterVertices;
        std::vector<uint32_t>       m_clusterTriangles;
        std::vector<GpuNodeDesc>    m_nodes;
        std::vector<MeshRange>      m_meshRanges;
        uint32_t m_skippedClusters = 0;

        Resource m_clusterDescBuffer;     // StructuredBuffer<ClusterDesc>
        Resource m_groupDescBuffer;       // StructuredBuffer<GroupDesc>
        Resource m_clusterVertexBuffer;   // Buffer<uint>
        Resource m_clusterTriangleBuffer; // Buffer<uint>
        Resource m_nodeDescBuffer;        // StructuredBuffer<NodeDesc>
        RhiBufferHandle m_clusterDescBufferHandle{};
        RhiBufferHandle m_groupDescBufferHandle{};
        RhiBufferHandle m_clusterVertexBufferHandle{};
        RhiBufferHandle m_clusterTriangleBufferHandle{};
        RhiBufferHandle m_nodeDescBufferHandle{};
        Resource* m_clusterDescBufferCompat     = nullptr;
        Resource* m_groupDescBufferCompat       = nullptr;
        Resource* m_clusterVertexBufferCompat   = nullptr;
        Resource* m_clusterTriangleBufferCompat = nullptr;
        Resource* m_nodeDescBufferCompat        = nullptr;
        IRHIDevice* m_device = nullptr;
    };
}

namespace SasamiEngine { using namespace Render; }
