#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class OccupancyBitGrid;

    // Dense float signed distance per voxel, in voxel units, same linear order as
    // OccupancyBitGrid (x fastest, then y, then z).
    //
    // Sign and offset convention (voxel-centre distances shifted by half a voxel):
    //   empty voxel    d = +(distance to the nearest occupied centre - 0.5)
    //   occupied voxel d = -(distance to the nearest empty centre    - 0.5)
    // so two face neighbours across the surface read +0.5 / -0.5 and the zero level sits on
    // the shared face. Only voxels inside the grid count as features: nothing outside the
    // grid is assumed to be empty or occupied. A grid without any occupied (or empty) voxel
    // has no feature for that side, and those voxels read +NoFeatureDistance()
    // (or -NoFeatureDistance()).
    class SignedDistanceGrid
    {
    public:
        SignedDistanceGrid() = default;

        // Any zero dimension makes the grid empty (all dims 0, no values).
        void Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ);

        uint32_t DimX() const { return m_dimX; }
        uint32_t DimY() const { return m_dimY; }
        uint32_t DimZ() const { return m_dimZ; }
        uint64_t VoxelCount() const { return static_cast<uint64_t>(m_dimX) * m_dimY * m_dimZ; }

        // Out-of-range coordinates: Set is a no-op, Get returns +NoFeatureDistance().
        void Set(uint32_t x, uint32_t y, uint32_t z, float value);
        float Get(uint32_t x, uint32_t y, uint32_t z) const;

        // Larger than any distance a real feature can produce in this grid (its diagonal + 1).
        float NoFeatureDistance() const;

        // Voxels whose (value < 0) disagrees with occupancy.Test. UINT64_MAX when dims differ.
        uint64_t CountSignMismatches(const OccupancyBitGrid& occupancy) const;

        uint64_t SizeInBytes() const { return static_cast<uint64_t>(m_values.size()) * sizeof(float); }
        const std::vector<float>& Values() const { return m_values; }
        std::vector<float>& MutableValues() { return m_values; }

    private:
        uint32_t m_dimX = 0;
        uint32_t m_dimY = 0;
        uint32_t m_dimZ = 0;
        std::vector<float> m_values;
    };
}

namespace SasamiEngine { using namespace Render; }
