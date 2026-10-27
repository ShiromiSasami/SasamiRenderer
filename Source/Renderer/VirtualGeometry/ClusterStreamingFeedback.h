#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace SasamiEngine::Render
{
    struct ClusterFeedbackReadout;
    struct ClusterStreamingStats
    {
        uint32_t viewsAccumulated = 0;     // views (cascades/main) merged this frame
        uint32_t visibleClustersTotal = 0; // sum over views
        uint32_t visibleClustersMax = 0;   // max over views
        uint32_t visibleRequestedMax = 0;  // max over views of the pre-clamp demand; exceeds visibleClustersMax exactly when a view's list overflowed
        uint32_t visibleCapacityMax = 0;   // max over views of the per-view visible-list capacity (entries)
        uint32_t visibleCapacityTotal = 0; // sum over views of that capacity: the visible-list footprint this frame's views actually hold
        uint32_t overflowViews = 0;        // views that reported any overflow
        uint32_t overflowFlags = 0;        // OR of every view's flags; bit meanings in MeshShader_Common.hlsli (kOverflow*)
        uint32_t wantedGroups = 0;         // popcount of the merged bitset
        uint32_t totalGroups = 0;          // groups in the scene (denominator)
        uint32_t newlyWantedGroups = 0;    // wanted now but not in the previous frame
        uint32_t droppedGroups = 0;        // wanted previously but not now
        uint32_t nodesVisited = 0;         // BVH nodes visited, summed over views (traversal work done)
        uint32_t instancesProcessed = 0;   // instance thread groups that ran, summed over views
        uint32_t maxTraversalLevels = 0;   // deepest per-instance level count seen (barrier round-trips)
        uint32_t clustersExamined = 0;     // cluster descs loaded at leaves, summed over views (the real leaf work)
    };
    // Merges the per-view GPU feedback of one frame into a scene-wide "wanted groups" set (the streaming
    // request set) and statistics. Karis 2021 ("Nanite: A Deep Dive", streaming): requests are the pages of the
    // clusters selected this frame; here the unit is the DAG group.
    class ClusterStreamingFeedback
    {
    public:
        // Overflow bits raised on the CPU rather than by ClusterSelect_CS, reported through the same
        // ClusterStreamingStats::overflowFlags field so that nothing that drops work is visible only
        // in the log. Continues the numbering of kOverflow* in MeshShader_Common.hlsli (1, 2, 4, 8);
        // keep the two in step, a value reused on both sides would report the wrong cause.
        static constexpr uint32_t kOverflowInstanceCapacity = 16u;  // the cluster instance buffer could not hold every instance

        void BeginFrame(uint32_t totalGroups);          // clears the current frame's accumulation, keeps the previous set for the delta
        void Accumulate(const ClusterFeedbackReadout& readout);   // OR the bitset in, sum/max the counts
        // ORs CPU-side flags into overflowFlags. Deliberately does not touch viewsAccumulated or
        // overflowViews: those count GPU views, and a CPU overflow is not one.
        void AccumulateCpuOverflow(uint32_t flags);
        void EndFrame();                                 // computes newly/dropped vs the previous frame, stores current as previous
        const ClusterStreamingStats& GetStats() const { return m_stats; }
        const std::vector<uint32_t>& GetWantedBits() const { return m_wanted; }   // 32 groups per word, size = ceil(totalGroups/32)
        bool IsGroupWanted(uint32_t group) const;
        std::string FormatReport() const;                // single line: "OK views=.. visibleTotal=.. .. nodes=.. instances=.. levels=.."
    private:
        std::vector<uint32_t> m_wanted, m_previous;
        ClusterStreamingStats m_stats{};
        bool m_frameOpen = false;
    };
}
namespace SasamiEngine { using namespace Render; }
