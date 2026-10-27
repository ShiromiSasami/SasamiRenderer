#include "ColumnSizing.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr uint32_t MAX_LOG2_CAP = 7;
        constexpr uint32_t UNREACHED = 0x3fffffffu;

        struct ColumnInfo
        {
            bool wet = false;
            float height = 0.0f;
            uint32_t top = 0;
            float velocity[3] = {0.0f, 0.0f, 0.0f};
        };

        void FillColumnInfo(const FlipCellGrid& cells, const Render::Voxel::StaggeredVelocityGrid& velocity,
                            bool velocityValid, uint32_t x, uint32_t z, ColumnInfo& info)
        {
            for (uint32_t y = cells.ny; y > 0; --y)
            {
                if (cells.At(x, y - 1, z) != FlipCell::Fluid)
                    continue;
                info.wet = true;
                info.top = y - 1;
                info.height = float(y);
                break;
            }
            if (!info.wet || !velocityValid)
                return;
            const uint32_t t = info.top;
            info.velocity[0] = 0.5f * (velocity.U(x, t, z) + velocity.U(x + 1, t, z));
            info.velocity[1] = 0.5f * (velocity.V(x, t, z) + velocity.V(x, t + 1, z));
            info.velocity[2] = 0.5f * (velocity.W(x, t, z) + velocity.W(x, t, z + 1));
        }

        bool HasSolidSideNeighbour(const FlipCellGrid& cells, uint32_t x, uint32_t z)
        {
            for (uint32_t y = 0; y < cells.ny; ++y)
            {
                if (cells.At(x, y, z) != FlipCell::Fluid)
                    continue;
                if ((x > 0 && cells.At(x - 1, y, z) == FlipCell::Solid) ||
                    (x + 1 < cells.nx && cells.At(x + 1, y, z) == FlipCell::Solid) ||
                    (z > 0 && cells.At(x, y, z - 1) == FlipCell::Solid) ||
                    (z + 1 < cells.nz && cells.At(x, y, z + 1) == FlipCell::Solid))
                    return true;
            }
            return false;
        }

        float VelocityDifference(const ColumnInfo& a, const ColumnInfo& b)
        {
            const float dx = a.velocity[0] - b.velocity[0];
            const float dy = a.velocity[1] - b.velocity[1];
            const float dz = a.velocity[2] - b.velocity[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    std::vector<uint8_t> ComputeColumnDesiredLog2(const FlipCellGrid& cells,
                                                  const Render::Voxel::StaggeredVelocityGrid& velocity,
                                                  const ColumnSizingSettings& settings, ColumnSizingStats* stats)
    {
        const uint32_t nx = cells.nx;
        const uint32_t ny = cells.ny;
        const uint32_t nz = cells.nz;
        if (stats)
            stats->markedColumns = 0;
        if (nx == 0 || ny == 0 || nz == 0)
            return {};

        const uint32_t maxLog2 = (std::min)(settings.maxLog2, MAX_LOG2_CAP);
        const bool velocityValid = velocity.DimX() == nx && velocity.DimY() == ny && velocity.DimZ() == nz;

        std::vector<ColumnInfo> columns(size_t(nx) * nz);
        for (uint32_t z = 0; z < nz; ++z)
            for (uint32_t x = 0; x < nx; ++x)
                FillColumnInfo(cells, velocity, velocityValid, x, z, columns[x + size_t(nx) * z]);

        std::vector<uint32_t> dist(columns.size(), UNREACHED);
        uint32_t marked = 0;
        for (uint32_t z = 0; z < nz; ++z)
        {
            for (uint32_t x = 0; x < nx; ++x)
            {
                const size_t idx = x + size_t(nx) * z;
                const ColumnInfo& self = columns[idx];
                if (!self.wet)
                    continue;

                const bool inside[4] = {x > 0, x + 1 < nx, z > 0, z + 1 < nz};
                const size_t nbrIdx[4] = {inside[0] ? idx - 1 : 0, inside[1] ? idx + 1 : 0,
                                          inside[2] ? idx - nx : 0, inside[3] ? idx + nx : 0};

                bool mark = false;
                bool allWet = true;
                float heightSum = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    if (!inside[i])
                    {
                        allWet = false;
                        continue;
                    }
                    const ColumnInfo& nbr = columns[nbrIdx[i]];
                    if (!nbr.wet)
                    {
                        allWet = false;
                        if (settings.refineWetDryEdge)
                            mark = true;
                        continue;
                    }
                    heightSum += nbr.height;
                    if (VelocityDifference(self, nbr) > settings.velocityThreshold)
                        mark = true;
                }
                if (allWet && std::fabs(heightSum - 4.0f * self.height) > settings.curvatureThreshold)
                    mark = true;
                if (!mark && settings.refineSolid && HasSolidSideNeighbour(cells, x, z))
                    mark = true;

                if (mark)
                {
                    dist[idx] = 0;
                    ++marked;
                }
            }
        }
        if (stats)
            stats->markedColumns = marked;

        // Two-pass chamfer, 8 neighbours, all weights 1: exact Chebyshev distance.
        if (marked > 0)
        {
            for (uint32_t z = 0; z < nz; ++z)
            {
                for (uint32_t x = 0; x < nx; ++x)
                {
                    uint32_t best = dist[x + size_t(nx) * z];
                    if (z > 0)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int px = int(x) + dx;
                            if (px < 0 || px >= int(nx))
                                continue;
                            best = (std::min)(best, dist[size_t(px) + size_t(nx) * (z - 1)] + 1);
                        }
                    }
                    if (x > 0)
                        best = (std::min)(best, dist[(x - 1) + size_t(nx) * z] + 1);
                    dist[x + size_t(nx) * z] = best;
                }
            }
            for (uint32_t z = nz; z > 0; --z)
            {
                const uint32_t cz = z - 1;
                for (uint32_t x = nx; x > 0; --x)
                {
                    const uint32_t cx = x - 1;
                    uint32_t best = dist[cx + size_t(nx) * cz];
                    if (cz + 1 < nz)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int px = int(cx) + dx;
                            if (px < 0 || px >= int(nx))
                                continue;
                            best = (std::min)(best, dist[size_t(px) + size_t(nx) * (cz + 1)] + 1);
                        }
                    }
                    if (cx + 1 < nx)
                        best = (std::min)(best, dist[(cx + 1) + size_t(nx) * cz] + 1);
                    dist[cx + size_t(nx) * cz] = best;
                }
            }
        }

        std::vector<uint8_t> desired(columns.size(), uint8_t(maxLog2));
        if (marked == 0)
            return desired;
        for (size_t i = 0; i < desired.size(); ++i)
        {
            const uint32_t d = (std::max)(1u, dist[i]);
            uint32_t level = 0;
            while (level < maxLog2 && (1u << (level + 1)) <= d)
                ++level;
            desired[i] = uint8_t(level);
        }
        return desired;
    }
}
