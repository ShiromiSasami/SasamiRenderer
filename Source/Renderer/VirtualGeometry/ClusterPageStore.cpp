#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterPageStore.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace SasamiEngine::Render
{
    namespace
    {
        bool IsClusterRangeValid(const GpuClusterDesc& cluster, size_t vertexCount, size_t triangleCount)
        {
            if (cluster.vertexCount == 0 && cluster.triangleCount == 0) {
                return true;
            }
            const uint64_t vertexEnd   = static_cast<uint64_t>(cluster.vertexOffset) + cluster.vertexCount;
            const uint64_t triangleEnd = static_cast<uint64_t>(cluster.triangleOffset) + cluster.triangleCount;
            return vertexEnd <= vertexCount && triangleEnd <= triangleCount;
        }
    }

    void ClusterPageStore::Build(const ClusterDagGpuBuffer& source)
    {
        Release();

        const std::vector<GpuGroupDesc>&   groups          = source.GetGroups();
        const std::vector<GpuClusterDesc>& sourceClusters  = source.GetClusters();
        const std::vector<uint32_t>&       sourceVertices  = source.GetClusterVertices();
        const std::vector<uint32_t>&       sourceTriangles = source.GetClusterTriangles();

        const uint32_t groupCount = static_cast<uint32_t>(groups.size());
        m_pages.resize(groupCount);

        struct PageLayout
        {
            uint32_t clusterCount  = 0;
            uint32_t vertexIdCount = 0;
            uint32_t triangleCount = 0;
            uint32_t skipped       = 0;
        };
        std::vector<PageLayout> layouts(groupCount);

        uint64_t totalBytes = 0;
        for (uint32_t g = 0; g < groupCount; ++g) {
            const GpuGroupDesc& group = groups[g];
            PageLayout& layout = layouts[g];
            layout.clusterCount = group.clusterCount;

            for (uint32_t c = 0; c < group.clusterCount; ++c) {
                const uint64_t clusterIndex = static_cast<uint64_t>(group.firstCluster) + c;
                if (clusterIndex >= sourceClusters.size()) {
                    ++layout.skipped;
                    continue;
                }
                const GpuClusterDesc& cluster = sourceClusters[clusterIndex];
                if (!IsClusterRangeValid(cluster, sourceVertices.size(), sourceTriangles.size())) {
                    ++layout.skipped;
                    continue;
                }
                layout.vertexIdCount  += cluster.vertexCount;
                layout.triangleCount  += cluster.triangleCount;
            }

            const uint32_t pageSize = PageSize(layout.clusterCount, layout.vertexIdCount, layout.triangleCount);
            totalBytes += pageSize;
        }

        m_bytes.reserve(totalBytes);
        m_bytes.resize(totalBytes);

        uint64_t writeOffset = 0;
        for (uint32_t g = 0; g < groupCount; ++g) {
            const GpuGroupDesc& group   = groups[g];
            const PageLayout&   layout  = layouts[g];
            const uint32_t vertexIdsOffset = VertexIdsOffset(layout.clusterCount);
            const uint32_t trianglesOffset = TrianglesOffset(layout.clusterCount, layout.vertexIdCount);
            const uint32_t pageSize        = PageSize(layout.clusterCount, layout.vertexIdCount, layout.triangleCount);

            uint8_t* page = m_bytes.data() + writeOffset;

            ClusterPageHeader header{};
            header.clusterCount  = layout.clusterCount;
            header.vertexIdCount = layout.vertexIdCount;
            header.triangleCount = layout.triangleCount;
            header.group         = g;
            std::memcpy(page, &header, sizeof(header));

            GpuClusterDesc* pageClusters = reinterpret_cast<GpuClusterDesc*>(page + sizeof(ClusterPageHeader));
            uint32_t*       pageVertices = reinterpret_cast<uint32_t*>(page + vertexIdsOffset);
            uint32_t*       pageTriangles = reinterpret_cast<uint32_t*>(page + trianglesOffset);

            uint32_t vertexCursor   = 0;
            uint32_t triangleCursor = 0;
            for (uint32_t c = 0; c < group.clusterCount; ++c) {
                const uint64_t clusterIndex = static_cast<uint64_t>(group.firstCluster) + c;
                if (clusterIndex >= sourceClusters.size()) {
                    pageClusters[c] = GpuClusterDesc{};
                    continue;
                }
                const GpuClusterDesc& srcCluster = sourceClusters[clusterIndex];
                if (!IsClusterRangeValid(srcCluster, sourceVertices.size(), sourceTriangles.size())) {
                    pageClusters[c] = GpuClusterDesc{};
                    continue;
                }

                GpuClusterDesc dstCluster = srcCluster;
                if (srcCluster.vertexCount == 0 && srcCluster.triangleCount == 0) {
                    pageClusters[c] = dstCluster;
                    continue;
                }

                dstCluster.vertexOffset   = vertexCursor;
                dstCluster.triangleOffset = triangleCursor;
                pageClusters[c] = dstCluster;

                std::memcpy(pageVertices + vertexCursor, sourceVertices.data() + srcCluster.vertexOffset,
                    static_cast<size_t>(srcCluster.vertexCount) * sizeof(uint32_t));
                std::memcpy(pageTriangles + triangleCursor, sourceTriangles.data() + srcCluster.triangleOffset,
                    static_cast<size_t>(srcCluster.triangleCount) * sizeof(uint32_t));

                vertexCursor   += srcCluster.vertexCount;
                triangleCursor += srcCluster.triangleCount;
            }

            ClusterPageEntry& entry = m_pages[g];
            entry.offset       = writeOffset;
            entry.size         = pageSize;
            entry.clusterCount = layout.clusterCount;

            writeOffset += pageSize;

            m_stats.clusterDescBytes += static_cast<uint64_t>(layout.clusterCount) * sizeof(GpuClusterDesc);
            m_stats.vertexIdBytes    += static_cast<uint64_t>(layout.vertexIdCount) * sizeof(uint32_t);
            m_stats.triangleBytes    += static_cast<uint64_t>(layout.triangleCount) * sizeof(uint32_t);
            m_stats.skippedClusters  += layout.skipped;
            m_stats.maxClustersPerPage = std::max(m_stats.maxClustersPerPage, layout.clusterCount);
            if (layout.clusterCount > kMaxClustersPerPage) {
                // The visible-list entry cannot address this many clusters within one page;
                // the in-page index would wrap. Only reachable with an extreme partitionSize.
                ++m_stats.oversizedPages;
            }
            m_stats.minPageBytes = m_stats.minPageBytes == 0 ? pageSize : std::min(m_stats.minPageBytes, pageSize);
            m_stats.maxPageBytes = std::max(m_stats.maxPageBytes, pageSize);
        }

        m_stats.pageCount  = groupCount;
        m_stats.totalBytes = totalBytes;
        m_stats.avgPageBytes = groupCount > 0
            ? static_cast<uint32_t>(totalBytes / groupCount)
            : 0u;
    }

    void ClusterPageStore::Release()
    {
        m_bytes.clear();
        m_pages.clear();
        m_stats = ClusterPageStoreStats{};
    }

    std::span<const uint8_t> ClusterPageStore::GetPageBytes(uint32_t group) const
    {
        const ClusterPageEntry& entry = m_pages[group];
        return std::span<const uint8_t>(m_bytes.data() + entry.offset, entry.size);
    }

    std::string ClusterPageStore::FormatReport() const
    {
        char buffer[320] = {};
        std::snprintf(buffer, sizeof(buffer),
            "OK pages=%u bytes=%.2fMB min=%u max=%u avg=%u maxClusters=%u descBytes=%llu vertexIdBytes=%llu triBytes=%llu skipped=%u oversizedPages=%u",
            m_stats.pageCount, static_cast<double>(m_stats.totalBytes) / (1024.0 * 1024.0),
            m_stats.minPageBytes, m_stats.maxPageBytes, m_stats.avgPageBytes, m_stats.maxClustersPerPage,
            static_cast<unsigned long long>(m_stats.clusterDescBytes),
            static_cast<unsigned long long>(m_stats.vertexIdBytes),
            static_cast<unsigned long long>(m_stats.triangleBytes),
            m_stats.skippedClusters, m_stats.oversizedPages);
        return buffer;
    }
}
