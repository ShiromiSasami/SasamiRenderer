#pragma once

#include "Renderer/Voxel/WindVelocityField.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Inertial particles carried by an IWindVelocityField: smoke puffs and falling petals.
    // Unlike WindParticleSystem's passive tracers they have their own velocity v, relaxed toward
    // the wind u by linear (Stokes) drag, dv/dt = (u - v) / dragTime + acceleration, integrated
    // exactly over a step for constant u:
    //   v' = u + a*tau + (v - u - a*tau) * exp(-dt / tau)
    // so a body falling in still air reaches the terminal speed |a| * tau. Smoke adds a buoyant
    // acceleration that fades with age and grows in size; petals add a sideways flutter force and
    // spin. A particle that ends a step inside solid is pushed back, loses the velocity component
    // into the wall (sticks with `wallFriction`), and keeps living; one that leaves the grid box
    // or outlives its lifetime is removed.
    enum class WindParticleKind : uint8_t
    {
        Smoke,
        Petal,
    };

    struct WindDrivenParticleSettings
    {
        WindParticleKind kind = WindParticleKind::Smoke;
        uint32_t maxParticles = 3000;
        // Continuous emission from a sphere at emitterPosition; rate 0 = only Emit() calls.
        float emitterPosition[3] = { 0.0f, 0.0f, 0.0f };
        float emitterRadius = 1.0f;             // world units
        float spawnRate = 150.0f;               // particles per second
        float minLifetime = 4.0f;               // seconds
        float maxLifetime = 8.0f;
        float dragTime = 0.5f;                  // tau, seconds
        float acceleration[3] = { 0.0f, 0.0f, 0.0f };  // constant (gravity), m/s^2
        float buoyancy = 0.0f;                  // m/s^2 upward (+y), scaled by (1 - age/lifetime)
        float initialVelocity[3] = { 0.0f, 0.0f, 0.0f };
        float velocityJitter = 0.5f;            // m/s, uniform per component
        float startSize = 0.5f;                 // world-space radius at birth
        float endSize = 2.0f;                   // at death (linear in age)
        float flutterAcceleration = 0.0f;       // m/s^2, petals only
        float flutterFrequency = 1.5f;          // Hz
        float spinRate = 6.0f;                  // rad/s, max |spin| (random sign), petals
        float wallFriction = 0.5f;              // fraction of the tangential velocity kept after a wall hit (1 = free slip)
    };

    struct WindDrivenParticleStats
    {
        uint32_t alive = 0;
        uint64_t spawned = 0;
        uint64_t expired = 0;       // lifetime
        uint64_t escaped = 0;       // left the grid box
        uint64_t wallHits = 0;      // pushed out of solid
        uint64_t dropped = 0;       // Emit/spawn requests refused at maxParticles
    };

    // One particle as WindParticleStep_CS.hlsl stores it (64 bytes, the HLSL WindParticle layout).
    // alive=0 particles are skipped; StepWindParticle never revives one.
    struct WindParticleState
    {
        float position[3];
        float age;
        float velocity[3];
        float lifetime;
        float angle;
        float spin;
        float phase;
        uint32_t alive;
        float flutterAxisX;
        float flutterAxisZ;
        uint32_t wallHits;
        uint32_t pad;
    };
    static_assert(sizeof(WindParticleState) == 64, "WindParticleState must match the HLSL WindParticle layout");

    // Everything one step needs besides the particle and the field; tau and decay are computed once
    // on the CPU so the GPU uses the exact same values.
    struct WindParticleStepParams
    {
        float dt = 0.0f;
        float tau = 0.0f;
        float decay = 0.0f;               // exp(-dt / tau)
        float voxelSize = 0.0f;
        float acceleration[3] = { 0.0f, 0.0f, 0.0f };
        float buoyancy = 0.0f;
        float flutterAcceleration = 0.0f; // 0 for smoke (flutter only runs for petals)
        float flutterFrequency = 0.0f;
        float wallFriction = 1.0f;
        uint32_t flutter = 0;             // 1 = petal with flutterAcceleration > 0
    };

    WindParticleStepParams MakeWindParticleStepParams(const WindDrivenParticleSettings& settings, float voxelSize, float dt);

    enum class WindParticleFate : uint8_t
    {
        Alive,
        Expired,    // age >= lifetime
        Escaped,    // left the grid box
    };

    // The per-particle body of WindDrivenParticles::Step (drag toward the wind, buoyancy/flutter,
    // spin, wall push-out). Sets state.alive = 0 on Expired/Escaped and increments wallHits on a push.
    // This is the CPU reference for WindParticleStep_CS.hlsl.
    WindParticleFate StepWindParticle(WindParticleState& state, const IWindVelocityField& wind,
                                      const WindParticleStepParams& params);

    class WindDrivenParticles
    {
    public:
        // Clears every particle and the stats. The RNG is a xorshift32 seeded with `seed` (0 -> 1).
        void Reset(const WindDrivenParticleSettings& settings, uint32_t seed);
        const WindDrivenParticleSettings& Settings() const { return m_settings; }
        // Moves the continuous emitter (e.g. to follow a fire) without touching live particles.
        void SetEmitterPosition(const float position[3]);
        void SetSpawnRate(float rate) { m_settings.spawnRate = rate; }

        // Spawns `count` particles inside a sphere of `radius` around `position` (refused past maxParticles).
        void Emit(const float position[3], float radius, uint32_t count);

        // Continuous emission (fractional carry between steps), then one drag/collision step for
        // every particle, then removal of dead ones (swap with last; order is not stable).
        void Step(const IWindVelocityField& wind, float dt);

        uint32_t Count() const { return static_cast<uint32_t>(m_age.size()); }
        const float* Position(uint32_t i) const { return &m_position[3 * static_cast<size_t>(i)]; }
        const float* Velocity(uint32_t i) const { return &m_velocity[3 * static_cast<size_t>(i)]; }
        float Age01(uint32_t i) const { return m_age[i] / m_lifetime[i]; }
        float Size(uint32_t i) const;           // world radius
        float Angle(uint32_t i) const { return m_angle[i]; }  // radians, petals spin
        // Petal flip: |cos| of the flutter phase, 1 = facing, 0 = edge-on.
        float Facing(uint32_t i) const;
        const WindDrivenParticleStats& Stats() const { return m_stats; }

        // Live particles as WindParticleState (alive=1, wallHits=0), in index order.
        void ExportStates(std::vector<WindParticleState>& outStates) const;
        // 6c-2: replaces every particle with the alive entries of `states` (in order; maxParticles is
        // not applied). Stats and the RNG are kept.
        void ImportStates(const std::vector<WindParticleState>& states);
        // Removes every particle, keeping settings, stats and the RNG.
        void ClearParticles();

    private:
        float Random01();
        void SpawnOne(const float centre[3], float radius);
        void Remove(uint32_t i);

        WindDrivenParticleSettings m_settings;
        WindDrivenParticleStats m_stats;
        uint32_t m_rng = 1;
        float m_spawnCarry = 0.0f;
        std::vector<float> m_position;
        std::vector<float> m_velocity;
        std::vector<float> m_age;
        std::vector<float> m_lifetime;
        std::vector<float> m_angle;
        std::vector<float> m_spin;
        std::vector<float> m_phase;             // flutter phase, radians
        std::vector<float> m_flutterAxis;       // unit xyz per particle, horizontal
    };
}
