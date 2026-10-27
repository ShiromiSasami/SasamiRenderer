#include "Renderer/ParticleCharacter/MolecularBody.h"

#include "Foundation/Jobs/ParallelFor.h"
#include "Renderer/Animation/SpringDamper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::ParticleCharacter
{
    namespace
    {
        constexpr uint32_t STEP_CHUNK_SIZE = 4096;
        constexpr float NO_PENDING_GOAL_CHANGE = 1e9f;

        // PCG output hash (Jarzynski & Olano 2020, "Hash Functions for GPU Rendering"), mapped to [0, 1).
        float HashToUnitFloat(uint32_t seed, uint32_t index)
        {
            uint32_t state = (seed * 0x9E3779B9u) ^ index;
            state = state * 747796405u + 2891336453u;
            const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
            const uint32_t h = (word >> 22u) ^ word;
            return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
        }
    }

    void MolecularBody::Reset(const float* targets, uint32_t count, uint32_t seed)
    {
        m_count = count;
        m_position.assign(targets, targets + size_t(count) * 3);
        m_velocity.assign(size_t(count) * 3, 0.0f);
        m_bond.assign(count, 1.0f);
        m_released.assign(count, 0);
        m_delayHash.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            m_delayHash[i] = HashToUnitFloat(seed, i);
        }
        m_bondGoal = 1.0f;
        m_sinceGoalChange = NO_PENDING_GOAL_CHANGE;
        m_floorHits = 0;
    }

    void MolecularBody::SetBondGoal(float goal)
    {
        m_bondGoal = (std::min)((std::max)(goal, 0.0f), 1.0f);
        m_sinceGoalChange = 0.0f;
        std::fill(m_released.begin(), m_released.end(), uint8_t(0));
    }

    void MolecularBody::Step(const float* targets, const float* normals, float dt,
                             const MolecularBodySettings& settings, bool parallel)
    {
        if (dt <= 0.0f || m_count == 0)
        {
            return;
        }

        const uint32_t numChunks = parallel ? (m_count + STEP_CHUNK_SIZE - 1) / STEP_CHUNK_SIZE : 1;
        std::vector<uint32_t> hits(numChunks, 0);
        if (parallel)
        {
            Foundation::ParallelFor(numChunks, [&](uint32_t c)
            {
                const uint32_t begin = c * STEP_CHUNK_SIZE;
                const uint32_t end = (std::min)(begin + STEP_CHUNK_SIZE, m_count);
                StepRange(begin, end, targets, normals, dt, settings, &hits[c]);
            });
        }
        else
        {
            StepRange(0, m_count, targets, normals, dt, settings, &hits[0]);
        }

        for (uint32_t h : hits)
        {
            m_floorHits += h;
        }
        m_sinceGoalChange += dt;
    }

    void MolecularBody::StepRange(uint32_t begin, uint32_t end, const float* targets, const float* normals,
                                  float dt, const MolecularBodySettings& settings, uint32_t* floorHits)
    {
        using Anim::Vec3;
        for (uint32_t i = begin; i < end; ++i)
        {
            Vec3 x{ m_position[i * 3 + 0], m_position[i * 3 + 1], m_position[i * 3 + 2] };
            Vec3 v{ m_velocity[i * 3 + 0], m_velocity[i * 3 + 1], m_velocity[i * 3 + 2] };
            const Vec3 t{ targets[i * 3 + 0], targets[i * 3 + 1], targets[i * 3 + 2] };
            const Vec3 n = normals != nullptr ? Vec3{ normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2] }
                                              : Vec3{ 0.0f, 1.0f, 0.0f };

            float delay = m_delayHash[i] * settings.bondDelaySpread
                        + settings.bondDelayPerHeight * (t.y - settings.centre[1]);
            delay = (std::max)(delay, 0.0f);
            const bool active = m_sinceGoalChange >= delay;

            float b = m_bond[i];
            if (active)
            {
                const float maxChange = settings.bondRate * dt;
                const float diff = m_bondGoal - b;
                b = std::fabs(diff) <= maxChange ? m_bondGoal : (diff > 0.0f ? b + maxChange : b - maxChange);
            }

            if (m_bondGoal < 0.5f && m_released[i] == 0 && active)
            {
                m_released[i] = 1;
                if (settings.force == MolecularForce::Scatter)
                {
                    v += n * settings.scatterSpeed;
                }
            }

            const float free = 1.0f - b;
            if (free > 0.0f)
            {
                Vec3 a{ 0.0f, -settings.gravity, 0.0f };
                if (settings.force == MolecularForce::Swirl)
                {
                    const float rx = x.x - settings.centre[0];
                    const float rz = x.z - settings.centre[2];
                    const float len = std::sqrt(rx * rx + rz * rz);
                    Vec3 tangent{ 0.0f, 0.0f, 0.0f };
                    if (len > 1e-6f)
                    {
                        tangent = Vec3{ -rz / len, 0.0f, rx / len };
                    }
                    const float speed = (std::min)(settings.swirlRate * len, 6.0f);
                    const Vec3 desired = tangent * speed + Vec3{ 0.0f, settings.swirlRise, 0.0f };
                    a = (desired - v) * 4.0f;
                }
                const float dtFree = dt * free;
                v += a * dtFree;
                v = v * std::exp(-settings.drag * dt * free);
                x += v * dtFree;
            }

            if (b > 1e-4f)
            {
                Anim::SimpleSpringDamper(x, v, t, settings.halflife / b, dt);
            }

            if (x.y < settings.floorY)
            {
                x.y = settings.floorY;
                if (v.y < 0.0f)
                {
                    v.y = -v.y * settings.restitution;
                }
                ++(*floorHits);
            }

            m_bond[i] = b;
            m_position[i * 3 + 0] = x.x;
            m_position[i * 3 + 1] = x.y;
            m_position[i * 3 + 2] = x.z;
            m_velocity[i * 3 + 0] = v.x;
            m_velocity[i * 3 + 1] = v.y;
            m_velocity[i * 3 + 2] = v.z;
        }
    }

    float MolecularBody::MaxTargetDistance(const float* targets) const
    {
        float maxDist = 0.0f;
        for (uint32_t i = 0; i < m_count; ++i)
        {
            const float dx = m_position[i * 3 + 0] - targets[i * 3 + 0];
            const float dy = m_position[i * 3 + 1] - targets[i * 3 + 1];
            const float dz = m_position[i * 3 + 2] - targets[i * 3 + 2];
            maxDist = (std::max)(maxDist, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        return maxDist;
    }
}
