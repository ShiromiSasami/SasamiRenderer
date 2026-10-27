#include "Renderer/Voxel/WindParticleSystem.h"

#include "Foundation/Jobs/ParallelFor.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr uint32_t CHUNK_SIZE = 4096;
        constexpr uint32_t MAX_SPAWN_TRIES = 32;

        uint32_t HashSeed(uint32_t seed, uint32_t index)
        {
            uint32_t h = seed ^ (index * 0x9E3779B9u);
            h ^= h >> 16;
            h *= 0x7FEB352Du;
            h ^= h >> 15;
            h *= 0x846CA68Bu;
            h ^= h >> 16;
            return h != 0 ? h : 0x6D2B79F5u;
        }

        float NextFloat01(uint32_t& state)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
        }

        float Length(const float v[3])
        {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }

        struct ChunkStats
        {
            WindParticleSystem::StepStats stats;
            double speedSum = 0.0;
            uint32_t live = 0;
        };
    }

    void WindParticleSystem::Reset(const IWindVelocityField& wind, uint32_t count, uint32_t seed, float minLifetime,
                                   float maxLifetime, float minSpawnDistance)
    {
        m_minLifetime = minLifetime;
        m_maxLifetime = maxLifetime;
        m_minSpawnDistance = minSpawnDistance;

        m_trail.assign(static_cast<size_t>(count) * TRAIL_LENGTH * 3, 0.0f);
        m_trailHead.assign(count, 0);
        m_trailCount.assign(count, 1);
        m_age.assign(count, 0.0f);
        m_lifetime.assign(count, 1.0f);
        m_speed.assign(count, 0.0f);
        m_rng.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            m_rng[i] = HashSeed(seed, i);
        }

        for (uint32_t i = 0; i < count; ++i) {
            Respawn(wind, i);
            m_age[i] = NextFloat01(m_rng[i]) * m_lifetime[i];
        }
    }

    void WindParticleSystem::Respawn(const IWindVelocityField& wind, uint32_t particle)
    {
        const VoxelGridDesc& desc = wind.Desc();
        const float extent[3] = { static_cast<float>(desc.dimX) * desc.voxelSize,
                                  static_cast<float>(desc.dimY) * desc.voxelSize,
                                  static_cast<float>(desc.dimZ) * desc.voxelSize };
        uint32_t& rng = m_rng[particle];

        float p[3] = { 0.0f, 0.0f, 0.0f };
        for (uint32_t attempt = 0; attempt < MAX_SPAWN_TRIES; ++attempt) {
            for (int axis = 0; axis < 3; ++axis) {
                p[axis] = desc.origin[axis] + NextFloat01(rng) * extent[axis];
            }
            if (wind.Distance(p) > m_minSpawnDistance) {
                break;
            }
        }

        m_lifetime[particle] = m_minLifetime + (m_maxLifetime - m_minLifetime) * NextFloat01(rng);
        m_age[particle] = 0.0f;

        float v[3];
        wind.Velocity(p, v);
        m_speed[particle] = Length(v);

        m_trailHead[particle] = 0;
        m_trailCount[particle] = 1;
        float* entry = &m_trail[static_cast<size_t>(particle) * TRAIL_LENGTH * 3];
        entry[0] = p[0];
        entry[1] = p[1];
        entry[2] = p[2];
    }

    WindParticleSystem::StepStats WindParticleSystem::Step(const IWindVelocityField& wind, float dt)
    {
        const uint32_t count = Count();
        const uint32_t chunkCount = (count + CHUNK_SIZE - 1) / CHUNK_SIZE;
        std::vector<ChunkStats> chunks(chunkCount);

        Foundation::ParallelFor(chunkCount, [&](uint32_t chunk) {
            ChunkStats& out = chunks[chunk];
            const uint32_t begin = chunk * CHUNK_SIZE;
            const uint32_t end = std::min(begin + CHUNK_SIZE, count);
            for (uint32_t i = begin; i < end; ++i) {
                const float* current = TrailPosition(i, 0);
                const float p[3] = { current[0], current[1], current[2] };

                float v1[3];
                wind.Velocity(p, v1);
                const float pm[3] = { p[0] + 0.5f * dt * v1[0], p[1] + 0.5f * dt * v1[1], p[2] + 0.5f * dt * v1[2] };
                float v2[3];
                wind.Velocity(pm, v2);
                const float pn[3] = { p[0] + dt * v2[0], p[1] + dt * v2[1], p[2] + dt * v2[2] };
                m_age[i] += dt;

                bool respawn = true;
                if (!wind.Contains(pn)) {
                    ++out.stats.respawnedOutside;
                } else if (wind.Distance(pn) < 0.0f) {
                    ++out.stats.respawnedSolid;
                } else if (m_age[i] > m_lifetime[i]) {
                    ++out.stats.respawnedAge;
                } else {
                    respawn = false;
                }

                if (respawn) {
                    Respawn(wind, i);
                } else {
                    bool push = true;
                    if (m_trailSpacing > 0.0f && m_trailCount[i] >= 2) {
                        const float* anchor = TrailPosition(i, 1);
                        const float d[3] = { pn[0] - anchor[0], pn[1] - anchor[1], pn[2] - anchor[2] };
                        push = Length(d) >= m_trailSpacing;
                    }
                    uint32_t head = m_trailHead[i];
                    if (push) {
                        head = (head + 1) % TRAIL_LENGTH;
                        m_trailHead[i] = head;
                        m_trailCount[i] = std::min(m_trailCount[i] + 1, TRAIL_LENGTH);
                    }
                    float* entry = &m_trail[(static_cast<size_t>(i) * TRAIL_LENGTH + head) * 3];
                    entry[0] = pn[0];
                    entry[1] = pn[1];
                    entry[2] = pn[2];
                    m_speed[i] = Length(v2);
                }

                const float speed = m_speed[i];
                out.speedSum += speed;
                out.stats.maxSpeed = std::max(out.stats.maxSpeed, speed);
                ++out.live;
            }
        });

        StepStats result;
        double speedSum = 0.0;
        uint32_t live = 0;
        for (const ChunkStats& chunk : chunks) {
            result.respawnedOutside += chunk.stats.respawnedOutside;
            result.respawnedSolid += chunk.stats.respawnedSolid;
            result.respawnedAge += chunk.stats.respawnedAge;
            result.maxSpeed = std::max(result.maxSpeed, chunk.stats.maxSpeed);
            speedSum += chunk.speedSum;
            live += chunk.live;
        }
        result.meanSpeed = live > 0 ? speedSum / static_cast<double>(live) : 0.0;
        return result;
    }

    const float* WindParticleSystem::TrailPosition(uint32_t particle, uint32_t k) const
    {
        const uint32_t index = (m_trailHead[particle] + TRAIL_LENGTH - k) % TRAIL_LENGTH;
        return &m_trail[(static_cast<size_t>(particle) * TRAIL_LENGTH + index) * 3];
    }
}
