#include "Renderer/Voxel/BrickOccupancyGrid.h"

#include <algorithm>
#include <array>
#include <bit>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        uint32_t BrickDim(uint32_t voxels)
        {
            return (voxels + BrickOccupancyGrid::BRICK_SIDE - 1) / BrickOccupancyGrid::BRICK_SIDE;
        }

        uint32_t BrickLocalBit(uint32_t x, uint32_t y, uint32_t z)
        {
            constexpr uint32_t SIDE = BrickOccupancyGrid::BRICK_SIDE;
            return (x % SIDE) + (y % SIDE) * SIDE + (z % SIDE) * SIDE * SIDE;
        }
    }

    void BrickOccupancyGrid::Build(const OccupancyBitGrid& dense)
    {
        m_dimX = dense.DimX();
        m_dimY = dense.DimY();
        m_dimZ = dense.DimZ();
        m_bricksX = BrickDim(m_dimX);
        m_bricksY = BrickDim(m_dimY);
        m_bricksZ = BrickDim(m_dimZ);
        m_fullBricks = 0;
        m_mixedBricks = 0;

        const uint64_t brickCount = BrickCount();
        const size_t maskWords = static_cast<size_t>((brickCount + 31) / 32);
        m_fullMask.assign(maskWords, 0u);
        m_mixedMask.assign(maskWords, 0u);
        m_mixedRank.assign(maskWords, 0u);
        m_payload.clear();

        std::array<uint32_t, BRICK_WORDS> brick{};
        uint64_t brickIndex = 0;
        for (uint32_t bz = 0; bz < m_bricksZ; ++bz)
        {
            for (uint32_t by = 0; by < m_bricksY; ++by)
            {
                for (uint32_t bx = 0; bx < m_bricksX; ++bx, ++brickIndex)
                {
                    brick.fill(0u);
                    uint32_t occupied = 0;
                    const uint32_t x0 = bx * BRICK_SIDE;
                    const uint32_t y0 = by * BRICK_SIDE;
                    const uint32_t z0 = bz * BRICK_SIDE;
                    const uint32_t x1 = (std::min)(x0 + BRICK_SIDE, m_dimX);
                    const uint32_t y1 = (std::min)(y0 + BRICK_SIDE, m_dimY);
                    const uint32_t z1 = (std::min)(z0 + BRICK_SIDE, m_dimZ);
                    for (uint32_t z = z0; z < z1; ++z)
                    {
                        for (uint32_t y = y0; y < y1; ++y)
                        {
                            for (uint32_t x = x0; x < x1; ++x)
                            {
                                if (dense.Test(x, y, z))
                                {
                                    const uint32_t bit = BrickLocalBit(x, y, z);
                                    brick[bit >> 5] |= 1u << (bit & 31u);
                                    ++occupied;
                                }
                            }
                        }
                    }

                    if (occupied == 0)
                    {
                        continue;
                    }
                    const uint32_t maskWord = static_cast<uint32_t>(brickIndex >> 5);
                    const uint32_t maskBit = 1u << (brickIndex & 31u);
                    if (occupied == BRICK_SIDE * BRICK_SIDE * BRICK_SIDE)
                    {
                        m_fullMask[maskWord] |= maskBit;
                        ++m_fullBricks;
                    }
                    else
                    {
                        m_mixedMask[maskWord] |= maskBit;
                        ++m_mixedBricks;
                        m_payload.insert(m_payload.end(), brick.begin(), brick.end());
                    }
                }
            }
        }

        uint32_t rank = 0;
        for (size_t w = 0; w < maskWords; ++w)
        {
            m_mixedRank[w] = rank;
            rank += static_cast<uint32_t>(std::popcount(m_mixedMask[w]));
        }
    }

    bool BrickOccupancyGrid::Test(uint32_t x, uint32_t y, uint32_t z) const
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ)
        {
            return false;
        }

        const uint64_t brickIndex = x / BRICK_SIDE
                                  + static_cast<uint64_t>(y / BRICK_SIDE) * m_bricksX
                                  + static_cast<uint64_t>(z / BRICK_SIDE) * m_bricksX * m_bricksY;
        const size_t maskWord = static_cast<size_t>(brickIndex >> 5);
        const uint32_t maskBit = static_cast<uint32_t>(brickIndex & 31u);
        if ((m_fullMask[maskWord] >> maskBit & 1u) != 0u)
        {
            return true;
        }
        if ((m_mixedMask[maskWord] >> maskBit & 1u) == 0u)
        {
            return false;
        }

        const uint32_t below = m_mixedMask[maskWord] & ((1u << maskBit) - 1u);
        const size_t slot = static_cast<size_t>(m_mixedRank[maskWord]) + static_cast<size_t>(std::popcount(below));
        const uint32_t bit = BrickLocalBit(x, y, z);
        return (m_payload[slot * BRICK_WORDS + (bit >> 5)] >> (bit & 31u) & 1u) != 0u;
    }

    void BrickOccupancyGrid::Decode(OccupancyBitGrid& out) const
    {
        out.Reset(m_dimX, m_dimY, m_dimZ);
        for (uint32_t z = 0; z < m_dimZ; ++z)
        {
            for (uint32_t y = 0; y < m_dimY; ++y)
            {
                for (uint32_t x = 0; x < m_dimX; ++x)
                {
                    if (Test(x, y, z))
                    {
                        out.Set(x, y, z);
                    }
                }
            }
        }
    }

    uint64_t BrickOccupancyGrid::TopLevelBytes() const
    {
        return static_cast<uint64_t>(m_fullMask.size() + m_mixedMask.size() + m_mixedRank.size()) * sizeof(uint32_t);
    }
}
