#include "Renderer/Voxel/WindFireSpread.h"

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr uint32_t MAX_TICKS_PER_STEP = 8;
        constexpr float MIN_TICK_INTERVAL = 1e-4f;
        constexpr float NORMAL_SCALE = 127.0f;

        // Face directions; +y comes first so a cell free on both +y and -y (grid floor) points up.
        constexpr int FACE_DIRS[6][3] = {
            { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
        };
    }

    void WindFireSpread::Build(const OccupancyBitGrid& grid, const VoxelGridDesc& desc, const WindFireSettings& settings, uint32_t seed)
    {
        m_desc = desc;
        m_desc.dimX = grid.DimX();
        m_desc.dimY = grid.DimY();
        m_desc.dimZ = grid.DimZ();
        m_settings = settings;
        m_rng = seed ? seed : 1u;
        m_tickCarry = 0.0f;
        m_stats = WindFireStats();
        m_burning.clear();
        m_front.clear();
        m_burnt.clear();
        m_igniteCells.clear();

        const uint32_t dimX = m_desc.dimX;
        const uint32_t dimY = m_desc.dimY;
        const uint32_t dimZ = m_desc.dimZ;
        const size_t count = static_cast<size_t>(dimX) * dimY * dimZ;
        m_state.assign(count, static_cast<uint8_t>(FireCellState::None));
        m_normal.assign(count * 3, 0);
        m_remaining.assign(count, 0.0f);

        for (uint32_t z = 0; z < dimZ; ++z)
            for (uint32_t y = 0; y < dimY; ++y)
                for (uint32_t x = 0; x < dimX; ++x)
                {
                    if (!grid.Test(x, y, z))
                        continue;
                    float sum[3] = { 0.0f, 0.0f, 0.0f };
                    const int* firstFree = nullptr;
                    for (const int* dir : FACE_DIRS)
                    {
                        const int nx = static_cast<int>(x) + dir[0];
                        const int ny = static_cast<int>(y) + dir[1];
                        const int nz = static_cast<int>(z) + dir[2];
                        const bool inside = nx >= 0 && ny >= 0 && nz >= 0
                            && nx < static_cast<int>(dimX) && ny < static_cast<int>(dimY) && nz < static_cast<int>(dimZ);
                        if (inside && grid.Test(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny), static_cast<uint32_t>(nz)))
                            continue;
                        if (!firstFree)
                            firstFree = dir;
                        for (int a = 0; a < 3; ++a)
                            sum[a] += static_cast<float>(dir[a]);
                    }
                    if (!firstFree)
                        continue;

                    float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    if (len < 1e-6f)
                    {
                        for (int a = 0; a < 3; ++a)
                            sum[a] = static_cast<float>(firstFree[a]);
                        len = 1.0f;
                    }
                    const uint32_t linear = x + y * dimX + z * dimX * dimY;
                    m_state[linear] = static_cast<uint8_t>(FireCellState::Unburnt);
                    for (int a = 0; a < 3; ++a)
                        m_normal[3 * static_cast<size_t>(linear) + a] = static_cast<int8_t>(std::lround(sum[a] / len * NORMAL_SCALE));
                    ++m_stats.surfaceCells;
                }
    }

    bool WindFireSpread::Ignite(const float worldPosition[3], uint32_t searchRadius)
    {
        if (!IsBuilt())
            return false;
        const int dims[3] = { static_cast<int>(m_desc.dimX), static_cast<int>(m_desc.dimY), static_cast<int>(m_desc.dimZ) };
        int base[3];
        for (int a = 0; a < 3; ++a)
            base[a] = static_cast<int>(std::floor((worldPosition[a] - m_desc.origin[a]) / m_desc.voxelSize));

        const int radius = static_cast<int>(searchRadius);
        int lo[3];
        int hi[3];
        for (int a = 0; a < 3; ++a)
        {
            lo[a] = (std::max)(base[a] - radius, 0);
            hi[a] = (std::min)(base[a] + radius, dims[a] - 1);
        }

        float bestDist = 0.0f;
        int64_t best = -1;
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x)
                {
                    const uint32_t linear = static_cast<uint32_t>(x) + static_cast<uint32_t>(y) * m_desc.dimX
                        + static_cast<uint32_t>(z) * m_desc.dimX * m_desc.dimY;
                    const FireCellState state = State(linear);
                    if (state != FireCellState::Unburnt && state != FireCellState::Burning)
                        continue;
                    const int cell[3] = { x, y, z };
                    float dist = 0.0f;
                    for (int a = 0; a < 3; ++a)
                    {
                        const float centre = m_desc.origin[a] + (static_cast<float>(cell[a]) + 0.5f) * m_desc.voxelSize;
                        const float d = centre - worldPosition[a];
                        dist += d * d;
                    }
                    if (best < 0 || dist < bestDist)
                    {
                        best = linear;
                        bestDist = dist;
                    }
                }
        if (best < 0)
            return false;

        const uint32_t linear = static_cast<uint32_t>(best);
        m_igniteCells.push_back(static_cast<int>(linear % m_desc.dimX));
        m_igniteCells.push_back(static_cast<int>((linear / m_desc.dimX) % m_desc.dimY));
        m_igniteCells.push_back(static_cast<int>(linear / (m_desc.dimX * m_desc.dimY)));
        if (State(linear) == FireCellState::Unburnt)
        {
            m_state[linear] = static_cast<uint8_t>(FireCellState::Burning);
            m_remaining[linear] = m_settings.burnTime;
            m_burning.push_back(linear);
            m_front.push_back(linear);
        }
        m_stats.burning = static_cast<uint32_t>(m_burning.size());
        return true;
    }

    void WindFireSpread::Extinguish()
    {
        for (uint8_t& state : m_state)
            if (state != static_cast<uint8_t>(FireCellState::None))
                state = static_cast<uint8_t>(FireCellState::Unburnt);
        std::fill(m_remaining.begin(), m_remaining.end(), 0.0f);
        m_burning.clear();
        m_front.clear();
        m_burnt.clear();
        m_igniteCells.clear();
        m_stats.burning = 0;
        m_stats.burnt = 0;
    }

    void WindFireSpread::Step(const IWindVelocityField& wind, float dt)
    {
        if (dt <= 0.0f || !IsBuilt())
            return;

        for (size_t i = 0; i < m_burning.size();)
        {
            const uint32_t linear = m_burning[i];
            m_remaining[linear] -= dt;
            if (m_remaining[linear] <= 0.0f)
            {
                m_remaining[linear] = 0.0f;
                m_state[linear] = static_cast<uint8_t>(FireCellState::Burnt);
                m_burnt.push_back(linear);
                m_burning[i] = m_burning.back();
                m_burning.pop_back();
            }
            else
            {
                ++i;
            }
        }

        if (m_settings.tickInterval > MIN_TICK_INTERVAL)
        {
            m_tickCarry += dt;
            uint32_t ticks = 0;
            while (m_tickCarry >= m_settings.tickInterval && ticks < MAX_TICKS_PER_STEP)
            {
                m_tickCarry -= m_settings.tickInterval;
                Tick(wind);
                ++ticks;
            }
        }
        m_stats.burning = static_cast<uint32_t>(m_burning.size());
        m_stats.burnt = static_cast<uint32_t>(m_burnt.size());
    }

    bool WindFireSpread::WithinSpreadRadius(int x, int y, int z) const
    {
        if (m_settings.maxSpreadRadius <= 0.0f)
            return true;
        const float r2 = m_settings.maxSpreadRadius * m_settings.maxSpreadRadius;
        for (size_t i = 0; i + 2 < m_igniteCells.size(); i += 3)
        {
            const float dx = static_cast<float>(x - m_igniteCells[i]);
            const float dy = static_cast<float>(y - m_igniteCells[i + 1]);
            const float dz = static_cast<float>(z - m_igniteCells[i + 2]);
            if (dx * dx + dy * dy + dz * dz <= r2)
                return true;
        }
        return false;
    }

    void WindFireSpread::Tick(const IWindVelocityField& wind)
    {
        ++m_stats.ticks;
        const int dimX = static_cast<int>(m_desc.dimX);
        const int dimY = static_cast<int>(m_desc.dimY);
        const int dimZ = static_cast<int>(m_desc.dimZ);
        const float voxel = m_desc.voxelSize;

        // Cells ignited during this tick spread on the next one.
        std::vector<uint32_t> source;
        source.swap(m_front);
        for (const uint32_t linear : source)
        {
            const int cx = static_cast<int>(linear % static_cast<uint32_t>(dimX));
            const int cy = static_cast<int>((linear / static_cast<uint32_t>(dimX)) % static_cast<uint32_t>(dimY));
            const int cz = static_cast<int>(linear / (static_cast<uint32_t>(dimX) * static_cast<uint32_t>(dimY)));
            const int cell[3] = { cx, cy, cz };

            float normal[3];
            OutwardNormal(linear, normal);
            float point[3];
            for (int a = 0; a < 3; ++a)
                point[a] = m_desc.origin[a] + (static_cast<float>(cell[a]) + 0.5f) * voxel + normal[a] * voxel;
            float u[3] = { 0.0f, 0.0f, 0.0f };
            if (wind.Contains(point))
                wind.Velocity(point, u);
            const float speed = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            const float windBoost = m_settings.p0 * std::exp(m_settings.c1 * speed);

            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (dx == 0 && dy == 0 && dz == 0)
                            continue;
                        const int nx = cx + dx;
                        const int ny = cy + dy;
                        const int nz = cz + dz;
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX || ny >= dimY || nz >= dimZ)
                            continue;
                        const uint32_t neighbour = static_cast<uint32_t>(nx) + static_cast<uint32_t>(ny) * m_desc.dimX
                            + static_cast<uint32_t>(nz) * m_desc.dimX * m_desc.dimY;
                        if (State(neighbour) != FireCellState::Unburnt || !WithinSpreadRadius(nx, ny, nz))
                            continue;

                        float cosTheta = 1.0f;
                        if (speed > 1e-6f)
                        {
                            const float invDir = 1.0f / std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));
                            cosTheta = (u[0] * dx + u[1] * dy + u[2] * dz) * invDir / speed;
                        }
                        float p = windBoost * std::exp(speed * m_settings.c2 * (cosTheta - 1.0f));
                        p = (std::min)((std::max)(p, 0.0f), 1.0f);
                        if (Random01() < p)
                        {
                            m_state[neighbour] = static_cast<uint8_t>(FireCellState::Burning);
                            m_remaining[neighbour] = m_settings.burnTime;
                            m_burning.push_back(neighbour);
                            m_front.push_back(neighbour);
                            ++m_stats.ignitions;
                        }
                    }
        }
        m_stats.burning = static_cast<uint32_t>(m_burning.size());
        m_stats.burnt = static_cast<uint32_t>(m_burnt.size());
    }

    float WindFireSpread::Random01()
    {
        uint32_t x = m_rng;
        x ^= x << 13u;
        x ^= x >> 17u;
        x ^= x << 5u;
        m_rng = x;
        return static_cast<float>(x >> 8u) * (1.0f / 16777216.0f);
    }

    float WindFireSpread::Remaining01(uint32_t linear) const
    {
        if (State(linear) != FireCellState::Burning || m_settings.burnTime <= 0.0f)
            return 0.0f;
        return (std::min)((std::max)(m_remaining[linear] / m_settings.burnTime, 0.0f), 1.0f);
    }

    void WindFireSpread::SurfacePoint(uint32_t linear, float outPosition[3]) const
    {
        const uint32_t dimX = m_desc.dimX;
        const uint32_t cell[3] = { linear % dimX, (linear / dimX) % m_desc.dimY, linear / (dimX * m_desc.dimY) };
        float normal[3];
        OutwardNormal(linear, normal);
        for (int a = 0; a < 3; ++a)
            outPosition[a] = m_desc.origin[a] + (static_cast<float>(cell[a]) + 0.5f) * m_desc.voxelSize
                + normal[a] * 0.5f * m_desc.voxelSize;
    }

    void WindFireSpread::OutwardNormal(uint32_t linear, float outNormal[3]) const
    {
        float n[3];
        for (int a = 0; a < 3; ++a)
            n[a] = static_cast<float>(m_normal[3 * static_cast<size_t>(linear) + a]) / NORMAL_SCALE;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int a = 0; a < 3; ++a)
            outNormal[a] = len > 1e-6f ? n[a] / len : 0.0f;
    }
}
