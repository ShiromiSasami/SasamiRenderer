#pragma once

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Sparse two-level form of an OccupancyBitGrid, after OpenVDB's leaf node and tile values
    // (Museth 2013): the grid is cut into 8x8x8 bricks; a 1-bit top-level mask marks the bricks
    // that are uniformly full (a tile, no payload) and a second one marks the mixed bricks,
    // whose 512 bits (16 words) are stored. Empty bricks cost only their two mask bits.
    // A mixed brick's payload is found by rank: a per-mask-word prefix count of set bits plus
    // a popcount within the word, so no per-brick index is stored.
    //
    // Brick-local layout: bit = lx + ly * 8 + lz * 64 (word = bit >> 5), the same x-fastest
    // order as the dense grid. Bricks along the grid's +X/+Y/+Z edge may hang over the grid;
    // their out-of-grid bits are zero, so such a brick is never "full".
    class BrickOccupancyGrid
    {
    public:
        static constexpr uint32_t BRICK_SIDE = 8;
        static constexpr uint32_t BRICK_WORDS = BRICK_SIDE * BRICK_SIDE * BRICK_SIDE / 32;

        // Replaces the contents with a compressed copy of `dense`.
        void Build(const OccupancyBitGrid& dense);

        uint32_t DimX() const { return m_dimX; }
        uint32_t DimY() const { return m_dimY; }
        uint32_t DimZ() const { return m_dimZ; }
        uint32_t BrickCountX() const { return m_bricksX; }
        uint32_t BrickCountY() const { return m_bricksY; }
        uint32_t BrickCountZ() const { return m_bricksZ; }
        uint64_t BrickCount() const { return static_cast<uint64_t>(m_bricksX) * m_bricksY * m_bricksZ; }
        uint64_t FullBrickCount() const { return m_fullBricks; }
        uint64_t MixedBrickCount() const { return m_mixedBricks; }

        // Out-of-range coordinates read as empty.
        bool Test(uint32_t x, uint32_t y, uint32_t z) const;

        // Expands back into a dense grid (resized to this grid's dimensions).
        void Decode(OccupancyBitGrid& out) const;

        // Storage, in bytes: the two top-level masks plus the rank table, and the mixed-brick
        // payload. Their sum is what replaces OccupancyBitGrid::SizeInBytes.
        uint64_t TopLevelBytes() const;
        uint64_t PayloadBytes() const { return static_cast<uint64_t>(m_payload.size()) * sizeof(uint32_t); }
        uint64_t SizeInBytes() const { return TopLevelBytes() + PayloadBytes(); }

    private:
        uint32_t m_dimX = 0;
        uint32_t m_dimY = 0;
        uint32_t m_dimZ = 0;
        uint32_t m_bricksX = 0;
        uint32_t m_bricksY = 0;
        uint32_t m_bricksZ = 0;
        uint64_t m_fullBricks = 0;
        uint64_t m_mixedBricks = 0;
        // One bit per brick, brick linear index = bx + by * bricksX + bz * bricksX * bricksY.
        std::vector<uint32_t> m_fullMask;
        std::vector<uint32_t> m_mixedMask;
        // m_mixedRank[w] = set bits in m_mixedMask[0..w), i.e. the payload slot of word w's
        // first mixed brick.
        std::vector<uint32_t> m_mixedRank;
        // BRICK_WORDS words per mixed brick, in brick linear order.
        std::vector<uint32_t> m_payload;
    };
}

namespace SasamiEngine { using namespace Render; }
