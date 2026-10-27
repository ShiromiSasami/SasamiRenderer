#include "Renderer/Voxel/SimGridSolidMask.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        struct AxisRange
        {
            uint32_t lo = 0;
            uint32_t hi = 0;
        };

        // Voxel indices v with voxMin < cellMax && voxMax > cellMin, clamped to [0, dim - 1]:
        //   v >= floor((cellMin - origin) / size)  and  v <= ceil((cellMax - origin) / size) - 1.
        // Returns false when no voxel on this axis qualifies.
        bool ComputeAxisRange(double voxelOrigin, double voxelSize, double cellMin, double cellMax,
                              uint32_t dim, AxisRange& out)
        {
            const double lo = (std::max)(std::floor((cellMin - voxelOrigin) / voxelSize), 0.0);
            const double hi = (std::min)(std::ceil((cellMax - voxelOrigin) / voxelSize) - 1.0,
                                         static_cast<double>(dim) - 1.0);
            if (lo > hi)
                return false;
            out.lo = static_cast<uint32_t>(lo);
            out.hi = static_cast<uint32_t>(hi);
            return true;
        }

        bool AnyOccupiedInBox(const OccupancyBitGrid& occupancy, const AxisRange& rx, const AxisRange& ry,
                              const AxisRange& rz)
        {
            for (uint32_t z = rz.lo; z <= rz.hi; ++z)
                for (uint32_t y = ry.lo; y <= ry.hi; ++y)
                    for (uint32_t x = rx.lo; x <= rx.hi; ++x)
                        if (occupancy.Test(x, y, z))
                            return true;
            return false;
        }

        bool ComputeCellRange(const VoxelGridDesc& voxelDesc, const SimGridDesc& sim, int axis, uint32_t index,
                              uint32_t dim, AxisRange& out)
        {
            const double cellSize = sim.cellSize;
            const double centre = static_cast<double>(sim.origin[axis]) + static_cast<double>(index) * cellSize;
            return ComputeAxisRange(voxelDesc.origin[axis], voxelDesc.voxelSize,
                                    centre - 0.5 * cellSize, centre + 0.5 * cellSize, dim, out);
        }
    }

    uint64_t BuildSimGridSolidMask(const VoxelGridDesc& voxelDesc,
                                   const OccupancyBitGrid& occupancy,
                                   const SimGridDesc& sim,
                                   std::vector<uint32_t>& outWords)
    {
        const uint64_t cellCount = static_cast<uint64_t>(sim.countX) * sim.countY * sim.countZ;
        outWords.assign(static_cast<size_t>((cellCount + 31) / 32), 0u);
        if (cellCount == 0 || !(sim.cellSize > 0.0f) || !(voxelDesc.voxelSize > 0.0f))
            return 0;
        if (occupancy.DimX() != voxelDesc.dimX || occupancy.DimY() != voxelDesc.dimY ||
            occupancy.DimZ() != voxelDesc.dimZ || occupancy.VoxelCount() == 0)
            return 0;

        uint64_t solidCount = 0;
        for (uint32_t k = 0; k < sim.countZ; ++k)
        {
            AxisRange rz;
            if (!ComputeCellRange(voxelDesc, sim, 2, k, voxelDesc.dimZ, rz))
                continue;
            for (uint32_t j = 0; j < sim.countY; ++j)
            {
                AxisRange ry;
                if (!ComputeCellRange(voxelDesc, sim, 1, j, voxelDesc.dimY, ry))
                    continue;
                for (uint32_t i = 0; i < sim.countX; ++i)
                {
                    AxisRange rx;
                    if (!ComputeCellRange(voxelDesc, sim, 0, i, voxelDesc.dimX, rx))
                        continue;
                    if (!AnyOccupiedInBox(occupancy, rx, ry, rz))
                        continue;
                    const uint64_t idx = i + static_cast<uint64_t>(j) * sim.countX +
                                         static_cast<uint64_t>(k) * sim.countX * sim.countY;
                    outWords[static_cast<size_t>(idx >> 5)] |= 1u << (idx & 31);
                    ++solidCount;
                }
            }
        }
        return solidCount;
    }
}
