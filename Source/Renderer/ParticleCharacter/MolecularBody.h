#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::ParticleCharacter
{
    enum class MolecularForce : uint8_t
    {
        Scatter,  // released particles fly out along their surface normal, with gravity
        Swirl,    // released particles orbit the vertical axis through `centre` and rise
        Melt,     // released particles fall and pool on the floor
    };

    struct MolecularBodySettings
    {
        float halflife = 0.06f;          // spring halflife (s) of a fully bonded particle toward its target
        float bondRate = 2.0f;           // bond change per second once a particle's delay has passed
        float bondDelaySpread = 0.6f;    // per-particle delay before its bond starts changing: hash * spread
        float bondDelayPerHeight = 0.0f; // extra delay per model unit above `centre[1]` (a wave up or down)
        MolecularForce force = MolecularForce::Scatter;
        float scatterSpeed = 2.5f;       // Scatter: initial speed along the normal when a particle releases
        float swirlRate = 3.0f;          // Swirl: tangential speed (units/s) per unit radius, capped below
        float swirlRise = 0.8f;          // Swirl: upward speed (units/s)
        float gravity = 9.8f;            // Scatter / Melt: downward acceleration
        float drag = 0.6f;               // free-motion velocity decay rate (1/s): v *= exp(-drag dt)
        float floorY = 0.0f;             // particles never go below it (Melt pools here)
        float restitution = 0.2f;        // normal velocity kept on a floor hit
        float centre[3] = { 0.0f, 0.0f, 0.0f };
    };

    // Particle body of a "molecular" character: every particle has a target on the animated surface
    // (SurfaceSampleSet interpolated over the current pose) and a bond in [0, 1]. A fully bonded
    // particle follows its target with Holden's exact critically damped spring; with bond b the
    // halflife is halflife / b, so b -> 0 lets go. The free part ((1 - b) of the force) is the chosen
    // MolecularForce plus drag. SetBondGoal(0) releases the body, SetBondGoal(1) re-forms it; each
    // particle starts moving its bond after its own delay, so the change ripples through the body.
    // The step is a per-particle function, so the serial and parallel steps are bit-identical (the GPU
    // twin, stage 2, has to match it the same way).
    class MolecularBody
    {
    public:
        // Places every particle on its target, at rest, with bond 1 and goal 1.
        void Reset(const float* targets, uint32_t count, uint32_t seed);
        uint32_t Count() const { return m_count; }

        // Starts moving every bond toward `goal` (0 or 1); per-particle delays restart now.
        void SetBondGoal(float goal);
        float BondGoal() const { return m_bondGoal; }

        // targets / normals: Count() * 3 floats of the current pose. parallel = ParallelFor over chunks.
        void Step(const float* targets, const float* normals, float dt, const MolecularBodySettings& settings,
                  bool parallel);

        const std::vector<float>& Positions() const { return m_position; }   // Count() * 3
        const std::vector<float>& Velocities() const { return m_velocity; }  // Count() * 3
        const std::vector<float>& Bonds() const { return m_bond; }           // Count()
        uint64_t FloorHits() const { return m_floorHits; }                   // cumulative over every Step
        const std::vector<float>& DelayHashes() const { return m_delayHash; } // Count(), fixed at Reset
        const std::vector<uint8_t>& Released() const { return m_released; }  // Count()
        float SinceGoalChange() const { return m_sinceGoalChange; }

        // Distance of the farthest particle from its target.
        float MaxTargetDistance(const float* targets) const;

    private:
        void StepRange(uint32_t begin, uint32_t end, const float* targets, const float* normals, float dt,
                       const MolecularBodySettings& settings, uint32_t* floorHits);

        uint32_t m_count = 0;
        std::vector<float> m_position;
        std::vector<float> m_velocity;
        std::vector<float> m_bond;
        std::vector<float> m_delayHash;   // per-particle hash in [0, 1), fixed at Reset
        std::vector<uint8_t> m_released;  // 1 once the particle got its release kick in this release
        float m_bondGoal = 1.0f;
        float m_sinceGoalChange = 0.0f;   // seconds since the last SetBondGoal
        uint64_t m_floorHits = 0;
    };
}

namespace SasamiEngine { using namespace Render; }
