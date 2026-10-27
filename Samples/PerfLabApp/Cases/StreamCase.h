#pragma once

#include "PerfCase.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelChunkStreamer.h"
#include "Renderer/Voxel/WorldTriangleGather.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // Whole-grid voxelization vs camera-centred chunk streaming (VoxelChunkStreamer, stage 6b).
    // The model's longest AABB side is `divisions` voxels; the anchor grid comes from the streamer.
    // Baseline = VoxelizeSurface of the whole anchor grid (+ BuildSignedDistanceField when sdfApron > 0).
    // Optimized = a fresh streamer (StartFromSoup), camera at the AABB centre, Tick + WaitIdle until
    // every chunk within `radius` is Loaded. The two do different amounts of work on purpose: the
    // point is what staying resident around the camera costs against holding the whole grid.
    // cpuMs = wall time; cpuBytes = full grid (+ float SDF) vs the streamer's residentBytes.
    // extras: voxelSize, dims, chunks, loaded, wanted, loads, startMs, lastJobMs, maxJobMs,
    // meanJobMs, residentSdfKB. Check: every loaded chunk equals the full grid's region bit for
    // bit and, with sdf, the full SDF clamped to +-(A - 0.5) exactly.
    class StreamCase : public IPerfCase {
    public:
        StreamCase(std::string name, std::string assetPath, uint32_t divisions, uint32_t chunkVoxels,
                   uint32_t radius, uint32_t sdfApron);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }
        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        PerfMetrics RunFull();
        PerfMetrics RunStream();

        std::string m_name;
        std::string m_assetPath;
        std::string m_description;
        uint32_t m_divisions = 0;
        Render::Voxel::VoxelChunkStreamerSettings m_settings{};
        std::shared_ptr<const Render::Voxel::WorldTriangleSoup> m_soup;
        Render::Voxel::VoxelGridDesc m_desc{};
        float m_camera[3] = {};
        // Reference built in Prepare (Check compares the last streamer against it).
        Render::Voxel::OccupancyBitGrid m_fullGrid;
        Render::Voxel::SignedDistanceGrid m_fullSdf;
        std::unique_ptr<Render::Voxel::VoxelChunkStreamer> m_streamer;
        bool m_streamComplete = false;
        bool m_fullMatches = true;
        std::vector<PerfExtraMetric> m_extras;
    };
}
