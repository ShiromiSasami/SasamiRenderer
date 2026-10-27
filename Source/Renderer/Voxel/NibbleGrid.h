#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class OccupancyBitGrid;

    // Dense grid of 4-bit cells (values 0..15), the multi-bit companion of OccupancyBitGrid.
    // Same linear order as OccupancyBitGrid (x fastest, then y, then z); cell i lives in word
    // i / 8 at bits 4 * (i % 8) .. 4 * (i % 8) + 3. 0 always means "empty", so the cells' nonzero
    // set is directly comparable with a 1-bit occupancy grid of the same dimensions.
    class NibbleGrid
    {
    public:
        NibbleGrid() = default;
        NibbleGrid(uint32_t dimX, uint32_t dimY, uint32_t dimZ);

        // Any zero dimension makes the grid empty (all dims 0, no words).
        void Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ);
        void Clear();

        uint32_t DimX() const { return m_dimX; }
        uint32_t DimY() const { return m_dimY; }
        uint32_t DimZ() const { return m_dimZ; }
        uint64_t CellCount() const { return static_cast<uint64_t>(m_dimX) * m_dimY * m_dimZ; }

        // Out-of-range coordinates: Set is a no-op, Get returns 0. Set keeps only value & 0xF.
        void Set(uint32_t x, uint32_t y, uint32_t z, uint8_t value);
        uint8_t Get(uint32_t x, uint32_t y, uint32_t z) const;

        uint64_t CountNonZero() const;
        // histogram[v] = number of cells holding v.
        std::array<uint64_t, 16> Histogram() const;
        // Cells whose (value != 0) disagrees with occupancy.Test at the same coordinate.
        // Returns UINT64_MAX when the dimensions differ.
        uint64_t CountNonZeroMismatches(const OccupancyBitGrid& occupancy) const;

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
