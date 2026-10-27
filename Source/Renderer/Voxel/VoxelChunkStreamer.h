#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelRegion.h"
#include "Renderer/Voxel/WorldTriangleGather.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SasamiEngine::Foundation
{
    class JobCounter;
}

namespace SasamiEngine::Render::Voxel
{
    struct VoxelChunkStreamerSettings
    {
        float voxelSize = 0.5f;          // world units per voxel
        uint32_t chunkVoxels = 32;       // C: a chunk is C^3 voxels (edge chunks are clamped to the grid)
        uint32_t loadRadius = 3;         // chunks within this Chebyshev chunk distance of the camera are loaded
        uint32_t unloadSlack = 1;        // unloaded once farther than loadRadius + unloadSlack (hysteresis)
        uint32_t maxJobsInFlight = 2;    // concurrent background voxelization jobs
        uint32_t padVoxels = 1;          // margin around the scene bounds when the anchor grid is built
        uint32_t sdfApronVoxels = 0;     // A > 0: each chunk also gets an SDF clamped to +-(A - 0.5) voxels (stage 2)
    };

    enum class VoxelChunkState : uint8_t
    {
        Loading,
        Loaded,
    };

    // One triangle's padded voxel box (inclusive), kept so the job gather does not recompute it.
    struct VoxelTriangleBox
    {
        uint16_t min[3] = {};
        uint16_t max[3] = {};
    };

    struct VoxelChunkStreamerStats
    {
        uint32_t chunkCountX = 0, chunkCountY = 0, chunkCountZ = 0;
        uint32_t loaded = 0;             // chunks resident now
        uint32_t loading = 0;            // jobs in flight
        uint32_t wanted = 0;             // chunks inside loadRadius at the last Tick
        uint64_t loadsCompleted = 0;     // cumulative
        uint64_t unloadsCompleted = 0;   // cumulative
        uint64_t discardedResults = 0;   // jobs that finished after their chunk was dropped
        uint64_t residentBytes = 0;      // occupancy + SDF bytes over loaded chunks
        uint64_t residentSdfBytes = 0;   // the SDF part of residentBytes
        double lastJobMs = 0.0;          // wall time of the last finished job (on its worker)
        double maxJobMs = 0.0;
        // Cumulative over loadsCompleted (PerfLab 5c): gather runs on the Tick thread, the rest on the job.
        double sumGatherMs = 0.0;        // GatherRegionTriangles inside the jobs (filter + sort)
        double sumVoxelizeMs = 0.0;      // VoxelizeChunk of the chunk (plus apron)
        double sumSdfMs = 0.0;           // BuildChunkSignedDistance (EDT + core copy), 0 when sdfApronVoxels = 0
        double sumJobMs = 0.0;
        uint64_t sumJobTriangles = 0;    // triangles handed to the jobs (a triangle counts once per job)
        uint64_t sumRegionVoxels = 0;    // voxels of the voxelized regions (chunk plus apron)
        uint64_t triangles = 0;          // in the shared soup
        int32_t cameraChunk[3] = { 0, 0, 0 };
    };

    // Camera-centred chunk streaming of surface occupancy (stage 1 of the 6b camera window).
    // Start() gathers the opaque scene triangles once into an immutable soup, fixes an anchor grid
    // (origin = scene min floored to voxelSize minus padVoxels, tight dims; edge chunks are clamped) and buckets the triangles
    // per chunk. Tick() loads the chunks within loadRadius of the camera, nearest first, on the
    // JobSystem (each job runs VoxelizeSurfaceRegion on its own chunk's triangles), and unloads the
    // ones past loadRadius + unloadSlack. Chunks use the anchor grid's global voxel placement, so
    // the union of all chunks equals a full VoxelizeSurface of the same desc bit for bit.
    // With sdfApronVoxels = A > 0 a job voxelizes the chunk plus an A-voxel apron and keeps the
    // chunk's SDF clamped to +-(A - 0.5) (ChunkSignedDistance.h); that equals the clamped SDF of
    // the whole anchor grid bit for bit.
    // Main thread only; workers write only into their own job record.
    class VoxelChunkStreamer
    {
    public:
        VoxelChunkStreamer();
        ~VoxelChunkStreamer();
        VoxelChunkStreamer(const VoxelChunkStreamer&) = delete;
        VoxelChunkStreamer& operator=(const VoxelChunkStreamer&) = delete;

        // Gathers the scene and builds the anchor grid; drops any previous state (waits for its jobs).
        bool Start(const std::vector<SceneSubmitter::DrawItem>& drawItems, const std::vector<Mesh>& meshes,
                   const VoxelChunkStreamerSettings& settings, std::string& outError);
        // Same as Start, from an already gathered soup (selftest / perf cases).
        bool StartFromSoup(std::shared_ptr<const WorldTriangleSoup> soup, const VoxelChunkStreamerSettings& settings,
                           std::string& outError);
        // Waits for in-flight jobs, then frees everything.
        void Stop();
        bool IsRunning() const { return m_running; }

        // Collects finished jobs, unloads far chunks, kicks loads for near ones.
        void Tick(const float cameraPosition[3]);
        // Blocks until no job is in flight and collects them (selftest / shutdown).
        void WaitIdle();

        const VoxelGridDesc& AnchorDesc() const { return m_desc; }
        const VoxelChunkStreamerSettings& Settings() const { return m_settings; }
        const VoxelChunkStreamerStats& Stats() const { return m_stats; }

        // Voxel region of chunk (cx, cy, cz) in the anchor grid; empty when out of range.
        VoxelRegion ChunkRegion(uint32_t cx, uint32_t cy, uint32_t cz) const;
        // Chunk containing a world point, clamped to the chunk range.
        void WorldToChunk(const float position[3], int32_t outChunk[3]) const;

        struct ChunkView
        {
            uint32_t coord[3];
            VoxelChunkState state;
            const OccupancyBitGrid* grid;   // null while Loading
            const SignedDistanceGrid* sdf;  // null while Loading or when sdfApronVoxels == 0
        };
        // Every resident or loading chunk (for drawing / tests).
        void ForEachChunk(const std::function<void(const ChunkView&)>& fn) const;

        // Synchronously voxelizes every chunk and the full anchor grid and compares them.
        // Writes "OK ..." or "ERR ..." to outReport. Expensive; keep the grid small.
        static bool SelfTest(std::string& outReport);

    private:
        struct Chunk;
        struct Job;

        uint32_t ChunkKey(uint32_t cx, uint32_t cy, uint32_t cz) const;
        // Chunk region widened by sdfApronVoxels and clamped to the grid.
        VoxelRegion ExpandedChunkRegion(uint32_t cx, uint32_t cy, uint32_t cz) const;
        // Sorted, unique triangles of the chunks overlapping region whose padded AABB touches region.
        // Called from the worker jobs.
        void GatherRegionTriangles(const VoxelRegion& region, std::vector<uint32_t>& outTriangles) const;
        bool RegionHasTriangles(const VoxelRegion& region) const;
        void RefreshResidentStats();
        void CollectFinished();
        void KickLoad(uint32_t cx, uint32_t cy, uint32_t cz);
        // Chunks overlapping region, inclusive.
        void ChunkRange(const VoxelRegion& region, uint32_t outLo[3], uint32_t outHi[3]) const;
        static void RunJob(void* userdata);

        VoxelChunkStreamerSettings m_settings{};
        VoxelGridDesc m_desc{};
        uint32_t m_chunkCount[3] = { 0, 0, 0 };
        std::shared_ptr<const WorldTriangleSoup> m_soup;
        std::vector<std::vector<uint32_t>> m_chunkTriangles;   // per chunk key: triangle indices into m_soup
        std::vector<VoxelTriangleBox> m_triangleBoxes;         // per triangle: padded voxel box (empty if dims > 65535)
        std::unordered_map<uint32_t, std::unique_ptr<Chunk>> m_chunks;
        std::vector<std::unique_ptr<Job>> m_jobs;              // in flight
        VoxelChunkStreamerStats m_stats{};
        bool m_running = false;
    };
}
