#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <algorithm>
#include <bit>

namespace SasamiEngine::Render::Voxel
{
    OccupancyBitGrid::OccupancyBitGrid(uint32_t dimX, uint32_t dimY, uint32_t dimZ)
    {
        Reset(dimX, dimY, dimZ);
    }

    void OccupancyBitGrid::Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ)
    {
        if (dimX == 0 || dimY == 0 || dimZ == 0)
        {
            m_dimX = 0;
            m_dimY = 0;
            m_dimZ = 0;
            m_words.clear();
            return;
        }

        m_dimX = dimX;
        m_dimY = dimY;
        m_dimZ = dimZ;
        const uint64_t wordCount = (VoxelCount() + 31u) / 32u;
        m_words.assign(static_cast<size_t>(wordCount), 0u);
    }

    void OccupancyBitGrid::Clear()
    {
        std::fill(m_words.begin(), m_words.end(), 0u);
    }

    void OccupancyBitGrid::Set(uint32_t x, uint32_t y, uint32_t z)
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ)
        {
            return;
        }

        const uint64_t linear = x + static_cast<uint64_t>(y) * m_dimX + static_cast<uint64_t>(z) * m_dimX * m_dimY;
        m_words[static_cast<size_t>(linear >> 5)] |= 1u << (linear & 31u);
    }

    void OccupancyBitGrid::Flip(uint32_t x, uint32_t y, uint32_t z)
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ)
        {
            return;
        }

        const uint64_t linear = x + static_cast<uint64_t>(y) * m_dimX + static_cast<uint64_t>(z) * m_dimX * m_dimY;
        m_words[static_cast<size_t>(linear >> 5)] ^= 1u << (linear & 31u);
    }

    bool OccupancyBitGrid::Test(uint32_t x, uint32_t y, uint32_t z) const
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ)
        {
            return false;
        }

        const uint64_t linear = x + static_cast<uint64_t>(y) * m_dimX + static_cast<uint64_t>(z) * m_dimX * m_dimY;
        return (m_words[static_cast<size_t>(linear >> 5)] >> (linear & 31u) & 1u) != 0u;
    }

    bool OccupancyBitGrid::OrWith(const OccupancyBitGrid& other)
    {
        if (other.m_dimX != m_dimX || other.m_dimY != m_dimY || other.m_dimZ != m_dimZ)
        {
            return false;
        }

        for (size_t i = 0; i < m_words.size(); ++i)
        {
            m_words[i] |= other.m_words[i];
        }
        return true;
    }

    bool OccupancyBitGrid::WriteRegion(const OccupancyBitGrid& src, const uint32_t dstMin[3])
    {
        if (static_cast<uint64_t>(dstMin[0]) + src.m_dimX > m_dimX
            || static_cast<uint64_t>(dstMin[1]) + src.m_dimY > m_dimY
            || static_cast<uint64_t>(dstMin[2]) + src.m_dimZ > m_dimZ)
        {
            return false;
        }

        for (uint32_t z = 0; z < src.m_dimZ; ++z)
        {
            for (uint32_t y = 0; y < src.m_dimY; ++y)
            {
                for (uint32_t x = 0; x < src.m_dimX; ++x)
                {
                    const uint64_t linear = static_cast<uint64_t>(x) + dstMin[0]
                        + (static_cast<uint64_t>(y) + dstMin[1]) * m_dimX
                        + (static_cast<uint64_t>(z) + dstMin[2]) * m_dimX * m_dimY;
                    const uint32_t mask = 1u << (linear & 31u);
                    uint32_t& word = m_words[static_cast<size_t>(linear >> 5)];
                    word = src.Test(x, y, z) ? (word | mask) : (word & ~mask);
                }
            }
        }
        return true;
    }

    uint64_t OccupancyBitGrid::CountOccupied() const
    {
        uint64_t count = 0;
        for (const uint32_t word : m_words)
        {
            count += static_cast<uint64_t>(std::popcount(word));
        }
        return count;
    }
}
