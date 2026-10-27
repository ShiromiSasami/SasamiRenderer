#include "OpticalLayerSizing.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        struct GroundedRun
        {
            bool valid = false;
            uint32_t bottom = 0;
            uint32_t top = 0;   // highest Fluid cell of the run that starts at the lowest non-Solid cell
        };

        GroundedRun FindGroundedRun(const FlipCellGrid& cells, uint32_t x, uint32_t z)
        {
            GroundedRun run;
            uint32_t bottom = 0;
            while (bottom < cells.ny && cells.At(x, bottom, z) == FlipCell::Solid)
                ++bottom;
            if (bottom >= cells.ny || cells.At(x, bottom, z) != FlipCell::Fluid)
                return run;
            uint32_t top = bottom;
            while (top + 1 < cells.ny && cells.At(x, top + 1, z) == FlipCell::Fluid)
                ++top;
            run.valid = true;
            run.bottom = bottom;
            run.top = top;
            return run;
        }

        void HorizontalVelocity(const Render::Voxel::StaggeredVelocityGrid& velocity, uint32_t x, uint32_t y,
                                uint32_t z, float out[2])
        {
            out[0] = 0.5f * (velocity.U(x, y, z) + velocity.U(x + 1, y, z));
            out[1] = 0.5f * (velocity.W(x, y, z) + velocity.W(x, y, z + 1));
        }

        float ColumnIndicator(const FlipCellGrid& cells, const Render::Voxel::StaggeredVelocityGrid& velocity,
                              const GroundedRun& run, uint32_t x, uint32_t z, float invRefSpeed)
        {
            float sum = 0.0f;
            for (uint32_t y = run.bottom + 1; y < run.top; ++y)
            {
                float lo[2];
                float mid[2];
                float hi[2];
                HorizontalVelocity(velocity, x, y - 1, z, lo);
                HorizontalVelocity(velocity, x, y, z, mid);
                HorizontalVelocity(velocity, x, y + 1, z, hi);
                const float a = hi[0] - 2.0f * mid[0] + lo[0];
                const float b = hi[1] - 2.0f * mid[1] + lo[1];
                sum += std::sqrt(a * a + b * b) * invRefSpeed;
            }
            return sum / float(cells.ny);
        }
    }

    std::vector<uint16_t> ComputeOpticalLayerBands(const FlipCellGrid& cells,
                                                   const Render::Voxel::StaggeredVelocityGrid& velocity,
                                                   const OpticalLayerSettings& settings, OpticalLayerStats* stats)
    {
        const uint32_t nx = cells.nx;
        const uint32_t nz = cells.nz;
        const size_t columnCount = size_t(nx) * nz;
        std::vector<uint16_t> bands(columnCount, 0);
        if (stats)
            *stats = OpticalLayerStats{};
        if (columnCount == 0 || cells.ny == 0)
            return bands;

        const bool velocityValid = velocity.DimX() == nx && velocity.DimY() == cells.ny && velocity.DimZ() == nz;
        const uint32_t minCells = (std::max)(settings.minCells, 1u);
        const uint32_t maxCells = (std::max)(settings.maxCells, minCells);
        const float invRefSpeed = 1.0f / (std::max)(settings.referenceSpeed, 1e-6f);

        std::vector<GroundedRun> runs(columnCount);
        std::vector<uint32_t> desired(columnCount, 0);
        float maxIndicator = 0.0f;
        for (uint32_t z = 0; z < nz; ++z)
        {
            for (uint32_t x = 0; x < nx; ++x)
            {
                const size_t c = x + size_t(nx) * z;
                runs[c] = FindGroundedRun(cells, x, z);
                if (!runs[c].valid)
                    continue;
                const float indicator =
                    velocityValid ? ColumnIndicator(cells, velocity, runs[c], x, z, invRefSpeed) : 0.0f;
                maxIndicator = (std::max)(maxIndicator, indicator);
                const float d = std::round(settings.scale * indicator);
                desired[c] = uint32_t(std::clamp(d, float(minCells), float(maxCells)));
            }
        }

        // Layer bottom per column (the tall-cell top): the lowest cell reached by the Manhattan dilation of
        // any surface cell, cell (x, y) is covered by (x', top') when |dx| + |dz| + (top' - y) < d'.
        std::vector<int> layerBottom(columnCount, 0);
        const int reach = int(maxCells) - 1;
        for (uint32_t z = 0; z < nz; ++z)
        {
            for (uint32_t x = 0; x < nx; ++x)
            {
                const size_t c = x + size_t(nx) * z;
                if (!runs[c].valid)
                    continue;
                int lowest = int(runs[c].top) + 1 - int(desired[c]);
                for (int dz = -reach; dz <= reach; ++dz)
                {
                    const int zz = int(z) + dz;
                    if (zz < 0 || zz >= int(nz))
                        continue;
                    const int rowReach = reach - std::abs(dz);
                    for (int dx = -rowReach; dx <= rowReach; ++dx)
                    {
                        const int xx = int(x) + dx;
                        if (xx < 0 || xx >= int(nx))
                            continue;
                        const size_t o = size_t(xx) + size_t(nx) * zz;
                        if (!runs[o].valid)
                            continue;
                        const int r = std::abs(dx) + std::abs(dz);
                        if (r >= int(desired[o]))
                            continue;
                        lowest = (std::min)(lowest, int(runs[o].top) + 1 - (int(desired[o]) - r));
                    }
                }
                layerBottom[c] = (std::max)(lowest, int(runs[c].bottom));
            }
        }

        std::vector<int> smoothed(columnCount, 0);
        for (uint32_t pass = 0; pass < settings.smoothIterations; ++pass)
        {
            for (uint32_t z = 0; z < nz; ++z)
            {
                for (uint32_t x = 0; x < nx; ++x)
                {
                    const size_t c = x + size_t(nx) * z;
                    smoothed[c] = layerBottom[c];
                    if (!runs[c].valid)
                        continue;
                    int sum = 0;
                    int count = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int xx = int(x) + dx;
                            const int zz = int(z) + dz;
                            if (xx < 0 || zz < 0 || xx >= int(nx) || zz >= int(nz))
                                continue;
                            const size_t o = size_t(xx) + size_t(nx) * zz;
                            if (!runs[o].valid)
                                continue;
                            sum += layerBottom[o];
                            ++count;
                        }
                    }
                    const int mean = int(std::floor(float(sum) / float(count)));
                    smoothed[c] = (std::max)((std::min)(layerBottom[c], mean), int(runs[c].bottom));
                }
            }
            layerBottom.swap(smoothed);
        }

        uint32_t minBand = UINT32_MAX;
        uint32_t maxBand = 0;
        uint64_t bandSum = 0;
        uint32_t wetColumns = 0;
        for (size_t c = 0; c < columnCount; ++c)
        {
            if (!runs[c].valid)
                continue;
            const uint32_t band = runs[c].top + 1 - uint32_t(layerBottom[c]);
            bands[c] = uint16_t((std::min)(band, 0xFFFFu));
            minBand = (std::min)(minBand, band);
            maxBand = (std::max)(maxBand, band);
            bandSum += band;
            ++wetColumns;
        }
        if (stats)
        {
            stats->minBand = wetColumns ? minBand : 0;
            stats->maxBand = maxBand;
            stats->meanBand = wetColumns ? float(double(bandSum) / wetColumns) : 0.0f;
            stats->maxIndicator = maxIndicator;
        }
        return bands;
    }
}
