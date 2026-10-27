#pragma once
#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    // 16 B, at page offset 0. Section offsets are derivable from this header:
    //   clusters   at 16
    //   vertexIds  at align16(16 + clusterCount * sizeof(GpuClusterDesc))
    //   triangles  at align16(vertexIdsOffset + vertexIdCount * 4)
    //   page size  = align16(trianglesOffset + triangleCount * 4)
    // GpuClusterDesc.vertexOffset/triangleOffset inside a page are rewritten to be ELEMENT
    // indices relative to that page's own vertex-id / triangle sections (not byte offsets, not
    // global indices), so a page is fully self-contained once copied into the GPU residency pool.
    struct ClusterPageHeader
    {
        uint32_t clusterCount   = 0;
        uint32_t vertexIdCount  = 0;
        uint32_t triangleCount  = 0;
        uint32_t group          = 0;
    };
    static_assert(sizeof(ClusterPageHeader) == 16);

    // Per group, into GetBytes().
    struct ClusterPageEntry
    {
        uint64_t offset       = 0;
        uint32_t size         = 0;
        uint32_t clusterCount = 0;
    };

    struct ClusterPageStoreStats
    {
        uint32_t pageCount           = 0;
        uint64_t totalBytes          = 0;
        uint32_t minPageBytes        = 0;
        uint32_t maxPageBytes        = 0;
        uint32_t avgPageBytes        = 0;
        uint32_t maxClustersPerPage  = 0;
        uint64_t clusterDescBytes    = 0;
        uint64_t vertexIdBytes       = 0;
        uint64_t triangleBytes       = 0;
        uint32_t skippedClusters     = 0;
        uint32_t oversizedPages      = 0;   // clusterCount > kMaxClustersPerPage (visible-entry index would wrap)
    };

    // RAM-resident page store: one page per DAG group holding everything the cluster path needs for that group
    // except the mesh vertex buffer. Pages are the streaming unit (Karis 2021 streams fixed-size pages of clusters;
    // vk_lod_clusters streams per group). Disk paging is deliberately not here yet: the DAG cache already loads in
    // ~0.2 s, so RAM -> GPU pool residency is the first target.
    class ClusterPageStore
    {
    public:
        static constexpr uint32_t kAlignment = 16;
        // Mirrors kVisibleClusterBits in MeshShader_Common.hlsli: the visible-list entry carries
        // the in-page cluster index in 8 bits, the other 24 going to the instance index. A group
        // produced by ClusterDagBuilder holds a few dozen clusters (21 at most across Bistro's
        // 8023 pages, against a partitionSize target of 16), so this is still headroom rather
        // than a real constraint -- Build reports any page that would exceed it (stats
        // oversizedPages) instead of letting the index wrap.
        // Keep in step with kVisibleClusterBits: widening one without the other silently
        // reintroduces the wrap this counter exists to catch.
        static constexpr uint32_t kMaxClustersPerPage = 0xFFu;

        static constexpr uint32_t Align16(uint32_t value)
        {
            return (value + (kAlignment - 1u)) & ~(kAlignment - 1u);
        }

        static constexpr uint32_t VertexIdsOffset(uint32_t clusterCount)
        {
            return Align16(static_cast<uint32_t>(sizeof(ClusterPageHeader)) + clusterCount * static_cast<uint32_t>(sizeof(GpuClusterDesc)));
        }

        static constexpr uint32_t TrianglesOffset(uint32_t clusterCount, uint32_t vertexIdCount)
        {
            return Align16(VertexIdsOffset(clusterCount) + vertexIdCount * 4u);
        }

        static constexpr uint32_t PageSize(uint32_t clusterCount, uint32_t vertexIdCount, uint32_t triangleCount)
        {
            return Align16(TrianglesOffset(clusterCount, vertexIdCount) + triangleCount * 4u);
        }

        // One page per group, in group order; clusters of a group are contiguous in source (firstCluster..).
        void Build(const ClusterDagGpuBuffer& source);
        void Release();

        bool IsBuilt() const { return !m_pages.empty(); }

        size_t GetPageCount() const { return m_pages.size(); }
        const ClusterPageEntry& GetPage(uint32_t group) const { return m_pages[group]; }
        std::span<const uint8_t> GetPageBytes(uint32_t group) const;
        const std::vector<uint8_t>& GetBytes() const { return m_bytes; }
        const ClusterPageStoreStats& GetStats() const { return m_stats; }

        std::string FormatReport() const;

    private:
        std::vector<uint8_t>          m_bytes;
        std::vector<ClusterPageEntry> m_pages;
        ClusterPageStoreStats         m_stats{};
    };
}

namespace SasamiEngine { using namespace Render; }
