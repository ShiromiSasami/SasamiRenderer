#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Dense 1-bit-per-voxel occupancy grid. 32 voxels are packed into one uint32_t.
    // The layout is the one the GPU voxelizer writes, so a CPU grid and a GPU readback can be
    // compared word for word:
    //   linear = x + y * dimX + z * dimX * dimY
    //   word   = linear >> 5,  bit = linear & 31
    class OccupancyBitGrid
    {
    public:
        OccupancyBitGrid() = default;
        OccupancyBitGrid(uint32_t dimX, uint32_t dimY, uint32_t dimZ);

        // Resizes and clears every bit. A zero dimension leaves an empty grid.
        void Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ);
        void Clear();

        uint32_t DimX() const { return m_dimX; }
        uint32_t DimY() const { return m_dimY; }
        uint32_t DimZ() const { return m_dimZ; }
        uint64_t VoxelCount() const { return static_cast<uint64_t>(m_dimX) * m_dimY * m_dimZ; }

        // Out-of-range coordinates are ignored by Set and read as empty by Test.
        void Set(uint32_t x, uint32_t y, uint32_t z);
        // Toggles one bit (out-of-range coordinates are ignored).
        void Flip(uint32_t x, uint32_t y, uint32_t z);
        bool Test(uint32_t x, uint32_t y, uint32_t z) const;

        // Sets every bit that is set in `other`. Fails (and changes nothing) unless both grids
        // have the same dimensions.
        bool OrWith(const OccupancyBitGrid& other);
        // Overwrites the box dstMin .. dstMin + src dims - 1 with `src` (bits set AND cleared).
        // Fails (and changes nothing) unless the box lies entirely inside this grid.
        bool WriteRegion(const OccupancyBitGrid& src, const uint32_t dstMin[3]);

        uint64_t CountOccupied() const;
        uint64_t SizeInBytes() const { return static_cast<uint64_t>(m_words.size()) * sizeof(uint32_t); }

        const std::vector<uint32_t>& Words() const { return m_words; }

    private:
        uint32_t m_dimX = 0;
        uint32_t m_dimY = 0;
        uint32_t m_dimZ = 0;
        std::vector<uint32_t> m_words;
    };
}

namespace SasamiEngine { using namespace Render; }
