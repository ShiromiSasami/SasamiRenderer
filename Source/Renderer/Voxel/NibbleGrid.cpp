#include "Renderer/Voxel/NibbleGrid.h"

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <algorithm>

namespace SasamiEngine::Render::Voxel
{
    NibbleGrid::NibbleGrid(uint32_t dimX, uint32_t dimY, uint32_t dimZ)
    {
        Reset(dimX, dimY, dimZ);
    }

    void NibbleGrid::Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ)
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
        const uint64_t wordCount = (CellCount() + 7u) / 8u;
        m_words.assign(static_cast<size_t>(wordCount), 0u);
    }

    void NibbleGrid::Clear()
    {
        std::fill(m_words.begin(), m_words.end(), 0u);
    }

    void NibbleGrid::Set(uint32_t x, uint32_t y, uint32_t z, uint8_t value)
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ)
        {
            return;
        }

        const uint64_t linear = x + static_cast<uint64_t>(y) * m_dimX + static_cast<uint64_t>(z) * m_dimX * m_dimY;
        const uint32_t shift = static_cast<uint32_t>(linear & 7u) * 4u;
        uint32_t& word = m_words[static_cast<size_t>(linear >> 3)];
        word = (word & ~(0xFu << shift)) | (static_cast<uint32_t>(value & 0xFu) << shift);
    }

    uint8_t NibbleGrid::Get(uint32_t x, uint32_t y, uint32_t z) const
    {
        if (x >= m_dimX || y >= m_dimY || z >= m_dimZ)
        {
            return 0;
        }

        const uint64_t linear = x + static_cast<uint64_t>(y) * m_dimX + static_cast<uint64_t>(z) * m_dimX * m_dimY;
        const uint32_t shift = static_cast<uint32_t>(linear & 7u) * 4u;
        return static_cast<uint8_t>((m_words[static_cast<size_t>(linear >> 3)] >> shift) & 0xFu);
    }

    uint64_t NibbleGrid::CountNonZero() const
    {
        const uint64_t cellCount = CellCount();
        uint64_t count = 0;
        for (uint64_t i = 0; i < cellCount; ++i)
        {
            const uint32_t shift = static_cast<uint32_t>(i & 7u) * 4u;
            if (((m_words[static_cast<size_t>(i >> 3)] >> shift) & 0xFu) != 0u)
            {
                ++count;
            }
        }
        return count;
    }

    std::array<uint64_t, 16> NibbleGrid::Histogram() const
    {
        std::array<uint64_t, 16> histogram{};
        const uint64_t cellCount = CellCount();
        for (uint64_t i = 0; i < cellCount; ++i)
        {
            const uint32_t shift = static_cast<uint32_t>(i & 7u) * 4u;
            ++histogram[(m_words[static_cast<size_t>(i >> 3)] >> shift) & 0xFu];
        }
        return histogram;
    }

    uint64_t NibbleGrid::CountNonZeroMismatches(const OccupancyBitGrid& occupancy) const
    {
        if (occupancy.DimX() != m_dimX || occupancy.DimY() != m_dimY || occupancy.DimZ() != m_dimZ)
        {
            return UINT64_MAX;
        }

        uint64_t mismatches = 0;
        uint64_t linear = 0;
        for (uint32_t z = 0; z < m_dimZ; ++z)
        {
            for (uint32_t y = 0; y < m_dimY; ++y)
            {
                for (uint32_t x = 0; x < m_dimX; ++x, ++linear)
                {
                    const uint32_t shift = static_cast<uint32_t>(linear & 7u) * 4u;
                    const bool nonZero = ((m_words[static_cast<size_t>(linear >> 3)] >> shift) & 0xFu) != 0u;
                    if (nonZero != occupancy.Test(x, y, z))
                    {
                        ++mismatches;
                    }
                }
            }
        }
        return mismatches;
    }
}
