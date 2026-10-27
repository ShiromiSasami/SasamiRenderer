#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterStreamingFeedback.h"

#include "Renderer/VirtualGeometry/ClusterGpuCulling.h"

#include <algorithm>
#include <bit>
#include <cstdio>

namespace SasamiEngine::Render
{
    namespace
    {
        uint32_t WordCountForGroups(uint32_t totalGroups)
        {
            return (totalGroups + 31u) / 32u;
        }
    }

    void ClusterStreamingFeedback::BeginFrame(uint32_t totalGroups)
    {
        const uint32_t wordCount = WordCountForGroups(totalGroups);
        m_wanted.assign(wordCount, 0u);
        if (m_previous.size() != wordCount) {
            m_previous.resize(wordCount, 0u);
        }

        m_stats = ClusterStreamingStats{};
        m_stats.totalGroups = totalGroups;
        m_frameOpen = true;
    }

    void ClusterStreamingFeedback::Accumulate(const ClusterFeedbackReadout& readout)
    {
        if (!m_frameOpen) {
            return;
        }

        const size_t wordCount = std::min(readout.wantedBits.size(), m_wanted.size());
        for (size_t i = 0; i < wordCount; ++i) {
            m_wanted[i] |= readout.wantedBits[i];
        }

        m_stats.viewsAccumulated += 1;
        m_stats.visibleClustersTotal += readout.visibleCount;
        m_stats.visibleClustersMax = std::max(m_stats.visibleClustersMax, readout.visibleCount);
        m_stats.visibleRequestedMax = std::max(m_stats.visibleRequestedMax, readout.requestedCount);
        m_stats.visibleCapacityMax = std::max(m_stats.visibleCapacityMax, readout.visibleCapacity);
        m_stats.visibleCapacityTotal += readout.visibleCapacity;
        if (readout.overflow) {
            m_stats.overflowViews += 1;
        }
        // Kept as the raw bit set the shader wrote, so the flag values stay defined in one place
        // (MeshShader_Common.hlsli) rather than being mirrored here to be named.
        m_stats.overflowFlags |= readout.overflow;
        m_stats.nodesVisited += readout.nodesVisited;
        m_stats.instancesProcessed += readout.instancesProcessed;
        m_stats.maxTraversalLevels = std::max(m_stats.maxTraversalLevels, readout.maxTraversalLevels);
        m_stats.clustersExamined += readout.clustersExamined;
    }

    void ClusterStreamingFeedback::AccumulateCpuOverflow(uint32_t flags)
    {
        if (!m_frameOpen) {
            return;
        }
        m_stats.overflowFlags |= flags;
    }

    void ClusterStreamingFeedback::EndFrame()
    {
        if (!m_frameOpen) {
            return;
        }
        m_frameOpen = false;

        if (m_stats.viewsAccumulated == 0) {
            // Nothing was accumulated this frame: leave the previous request set as-is for the
            // next delta and report a zeroed frame (totalGroups is the only meaningful field).
            return;
        }

        uint32_t wantedGroups = 0;
        uint32_t newlyWanted   = 0;
        uint32_t dropped       = 0;
        const size_t wordCount = std::min(m_wanted.size(), m_previous.size());
        for (size_t i = 0; i < wordCount; ++i) {
            const uint32_t current  = m_wanted[i];
            const uint32_t previous = m_previous[i];
            wantedGroups += static_cast<uint32_t>(std::popcount(current));
            newlyWanted  += static_cast<uint32_t>(std::popcount(current & ~previous));
            dropped      += static_cast<uint32_t>(std::popcount(previous & ~current));
        }

        m_stats.wantedGroups      = wantedGroups;
        m_stats.newlyWantedGroups = newlyWanted;
        m_stats.droppedGroups     = dropped;

        m_previous = m_wanted;
    }

    bool ClusterStreamingFeedback::IsGroupWanted(uint32_t group) const
    {
        const size_t wordIndex = group / 32u;
        if (wordIndex >= m_wanted.size()) {
            return false;
        }
        return (m_wanted[wordIndex] >> (group % 32u)) & 1u;
    }

    std::string ClusterStreamingFeedback::FormatReport() const
    {
        const double percentage = m_stats.totalGroups > 0
            ? (100.0 * static_cast<double>(m_stats.wantedGroups) / static_cast<double>(m_stats.totalGroups))
            : 0.0;

        char buffer[512] = {};
        std::snprintf(buffer, sizeof(buffer),
            "OK views=%u visibleTotal=%u visibleMax=%u visibleReqMax=%u visibleCapMax=%u visibleCapTotal=%u overflowViews=%u overflowFlags=0x%X wantedGroups=%u/%u (%.1f%%) newlyWanted=%u dropped=%u"
            " | nodes=%u instances=%u levels=%u examined=%u",
            m_stats.viewsAccumulated, m_stats.visibleClustersTotal, m_stats.visibleClustersMax,
            m_stats.visibleRequestedMax, m_stats.visibleCapacityMax, m_stats.visibleCapacityTotal,
            m_stats.overflowViews,
            m_stats.overflowFlags,
            m_stats.wantedGroups, m_stats.totalGroups, percentage, m_stats.newlyWantedGroups, m_stats.droppedGroups,
            m_stats.nodesVisited, m_stats.instancesProcessed, m_stats.maxTraversalLevels, m_stats.clustersExamined);
        return buffer;
    }
}
