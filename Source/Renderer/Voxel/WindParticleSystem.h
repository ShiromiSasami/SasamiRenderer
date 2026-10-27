#pragma once

#include "Renderer/Voxel/WindVelocityField.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Passive tracer particles advected through an IWindVelocityField (midpoint / RK2). A particle is
    // respawned at a random free point of the grid box when it leaves the box, ends a step inside
    // a solid (d < 0, counted as a penetration) or exceeds its lifetime. Each particle keeps the
    // last TRAIL_LENGTH positions for streak drawing. Deterministic for a given seed, also when
    // stepped in parallel (one xorshift state per particle).
    class WindParticleSystem
    {
    public:
        static constexpr uint32_t TRAIL_LENGTH = 8;

        struct StepStats
        {
            uint32_t respawnedOutside = 0;
            uint32_t respawnedSolid = 0;     // penetrations
            uint32_t respawnedAge = 0;
            double meanSpeed = 0.0;          // m/s over live particles after the step
            float maxSpeed = 0.0f;
        };

        // Spawns `count` particles at free points (d > minSpawnDistance, world units).
        void Reset(const IWindVelocityField& wind, uint32_t count, uint32_t seed, float minLifetime, float maxLifetime,
                   float minSpawnDistance);
        // Advances every particle by dt seconds. Trails shift by one entry per step, or, with a trail
        // spacing > 0, only once the head is that far (world units) from entry 1; until then the step
        // overwrites entry 0. Small per-frame steps then still give long trails.
        StepStats Step(const IWindVelocityField& wind, float dt);
        void SetTrailSpacing(float spacing) { m_trailSpacing = spacing; }

        uint32_t Count() const { return static_cast<uint32_t>(m_age.size()); }
        // Trail entry k of particle i (k = 0 newest). xyz.
        const float* TrailPosition(uint32_t particle, uint32_t k) const;
        // Number of valid trail entries (resets to 1 on respawn).
        uint32_t TrailCount(uint32_t particle) const { return m_trailCount[particle]; }
        float Speed(uint32_t particle) const { return m_speed[particle]; }

    private:
        void Respawn(const IWindVelocityField& wind, uint32_t particle);

        std::vector<float> m_trail;          // count * TRAIL_LENGTH * 3, ring per particle
        std::vector<uint32_t> m_trailHead;   // index of the newest entry
        std::vector<uint32_t> m_trailCount;
        std::vector<float> m_age;
        std::vector<float> m_lifetime;
        std::vector<float> m_speed;
        std::vector<uint32_t> m_rng;
        float m_minLifetime = 2.0f;
        float m_maxLifetime = 6.0f;
        float m_minSpawnDistance = 0.0f;
        float m_trailSpacing = 0.0f;
    };
}

namespace SasamiEngine { using namespace Render; }
