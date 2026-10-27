#pragma once

#include "PerfCase.h"
#include "Renderer/Voxel/BrickOccupancyGrid.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // Dense 1-bit occupancy grid vs its 8^3 brick form (BrickOccupancyGrid, OpenVDB-style tiles
    // and leaves, Museth 2013). Prepare voxelizes the model (surface, or surface OR parity solid)
    // with the longest AABB side at `maxDim` voxels and builds the bricks once.
    // Baseline = OccupancyBitGrid::Test, Optimized = BrickOccupancyGrid::Test, both over the same
    // fixed query list (half uniform in the grid, half within 2 voxels of an occupied voxel, so the
    // empty-brick shortcut does not decide the result alone). cpuMs = query time; cpuBytes = the
    // grid's storage. extras: denseKB, brickKB, ratio, full/mixed bricks, voxelizeMs, buildMs,
    // decodeMs. Check: both variants count the same hits and Decode equals the dense grid word for word.
    class BrickGridCase : public IPerfCase {
    public:
        BrickGridCase(std::string name, std::string assetPath, uint32_t maxDim, bool solid);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }
        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        std::string m_name;
        std::string m_assetPath;
        std::string m_description;
        uint32_t m_maxDim = 0;
        bool m_solid = false;
        Render::Voxel::OccupancyBitGrid m_dense;
        Render::Voxel::BrickOccupancyGrid m_bricks;
        // Query coordinates packed as x | y << 10 | z << 20 (maxDim <= 1024).
        std::vector<uint32_t> m_queries;
        std::vector<PerfExtraMetric> m_extras;
        uint64_t m_hits[2] = {};
        bool m_ran[2] = {};
        bool m_decodeOk = false;
    };
}
