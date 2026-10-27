#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace SasamiEngine::Render
{
    struct ClusterResidencyStats
    {
        uint32_t totalGroups = 0;
        uint32_t residentGroups = 0;
        uint32_t pinnedGroups = 0;
        uint32_t budgetGroups = 0;        // 0 = unlimited (everything resident)
        uint32_t maxLoadsPerFrame = 0;
        uint32_t retentionFrames = 0;     // a group wanted within this many frames is not evicted
        uint32_t loadsThisFrame = 0;
        uint32_t evictionsThisFrame = 0;
        uint32_t missingWanted = 0;       // wanted this frame but still not resident after Update (budget/cap)

        // Frames a group waited between first appearing in the wanted set and becoming resident.
        // The wanted set itself is already delayed by the feedback readback, so this measures the
        // residency queue only; add ClusterGpuCulling's readback delay for the end-to-end figure.
        // 0 is the normal value when the budget allows an immediate load.
        uint32_t waitFramesLast = 0;
        uint32_t waitFramesMax  = 0;
        uint64_t waitSamples    = 0;
        uint64_t waitFramesTotal = 0;
        uint64_t totalLoads = 0;
        uint64_t totalEvictions = 0;
    };
    // Residency policy of the cluster-DAG groups (Nanite-style streaming, Karis 2021; vk_lod_clusters: requests are
    // fulfilled between traversal and rendering, the coarser level covers meanwhile). Data movement is not here:
    // this class only decides the resident set; the GPU select shader falls back to coarser clusters for
    // non-resident groups.
    class ClusterResidencyManager
    {
    public:
        // Rebuilds the state for a scene. pinnedBits (32 groups per word) marks groups that must always stay resident
        // (the terminal/root groups of every mesh). With budget 0 every group starts resident; otherwise only pinned.
        void Reset(uint32_t totalGroups, const std::vector<uint32_t>& pinnedBits);
        void SetBudget(uint32_t maxResidentGroups);      // 0 = unlimited: makes everything resident immediately
        void SetMaxLoadsPerFrame(uint32_t loads);        // default 256; 0 = unlimited
        // Hysteresis. Without it the request set never settles under a budget: a non-resident group is
        // drawn by its coarser parent, which makes the parent wanted too; loading the fine group drops
        // the parent, evicting the parent brings it back. Measured on Bistro at budget 2500 with a static
        // camera: the wanted set swung 4837-5009 and 103-256 groups were reloaded every frame forever.
        // Keeping recently wanted groups resident breaks that loop. 0 = evict purely by last-wanted order.
        void SetRetentionFrames(uint32_t frames);        // default 60
        // One frame of feedback: stamps wanted groups, loads wanted non-resident groups (respecting the budget and the
        // per-frame cap, evicting the least-recently-wanted non-pinned resident groups when over budget).
        void Update(const std::vector<uint32_t>& wantedBits, uint64_t frameIndex);
        const std::vector<uint32_t>& GetResidentBits() const { return m_resident; }
        bool IsResident(uint32_t group) const;
        const ClusterResidencyStats& GetStats() const { return m_stats; }
        std::string FormatReport() const;  // "resident=../total pinned=.. budget=.. loads=..(+total) evictions=..(+total) missing=.."
    private:
        std::vector<uint32_t> m_resident, m_pinned;
        std::vector<uint64_t> m_lastWanted;   // per group, frame index of the last request (0 = never)
        std::vector<uint64_t> m_pendingSince; // per group, frame index it was first wanted while
                                              // non-resident (0 = nothing outstanding)
        ClusterResidencyStats m_stats{};
        uint32_t m_residentCount = 0;
        uint32_t m_retentionFrames = 60;
    };
}
namespace SasamiEngine { using namespace Render; }
