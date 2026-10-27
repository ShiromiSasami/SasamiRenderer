#include "Renderer/Voxel/SignedDistanceGrid.h"

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    void SignedDistanceGrid::Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ)
    {
        if (dimX == 0 || dimY == 0 || dimZ == 0) {
            dimX = dimY = dimZ = 0;
        }
        m_dimX = dimX;
        m_dimY = dimY;
        m_dimZ = dimZ;
        m_values.assign(static_cast<size_t>(VoxelCount()), 0.0f);
    }

    void SignedDistanceGrid::Set(uint32_t x, uint32_t y, uint32_t z, float value)
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ) {
            return;
        }
        m_values[x + static_cast<size_t>(y) * m_dimX + static_cast<size_t>(z) * m_dimX * m_dimY] = value;
    }

    float SignedDistanceGrid::Get(uint32_t x, uint32_t y, uint32_t z) const
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ) {
            return NoFeatureDistance();
        }
        return m_values[x + static_cast<size_t>(y) * m_dimX + static_cast<size_t>(z) * m_dimX * m_dimY];
    }

    float SignedDistanceGrid::NoFeatureDistance() const
    {
        const double diagonal = std::sqrt(static_cast<double>(m_dimX) * m_dimX
                                          + static_cast<double>(m_dimY) * m_dimY
                                          + static_cast<double>(m_dimZ) * m_dimZ);
        return static_cast<float>(diagonal + 1.0);
    }

    uint64_t SignedDistanceGrid::CountSignMismatches(const OccupancyBitGrid& occupancy) const
    {
        if (occupancy.DimX() != m_dimX || occupancy.DimY() != m_dimY || occupancy.DimZ() != m_dimZ) {
            return UINT64_MAX;
        }
        uint64_t mismatches = 0;
        size_t i = 0;
        for (uint32_t z = 0; z < m_dimZ; ++z) {
            for (uint32_t y = 0; y < m_dimY; ++y) {
                for (uint32_t x = 0; x < m_dimX; ++x, ++i) {
                    if ((m_values[i] < 0.0f) != occupancy.Test(x, y, z)) {
                        ++mismatches;
                    }
                }
            }
        }
        return mismatches;
    }
}
