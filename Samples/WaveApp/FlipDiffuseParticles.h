#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    class FlipWaterSolver;

    enum class DiffuseKind : uint8_t
    {
        Spray = 0,
        Foam = 1,
        Bubble = 2,
    };

    struct FlipDiffuseSettings
    {
        bool enabled = true;
        // Clamping thresholds of eq.1 (Ihmsen, Akinci, Akinci & Teschner 2012). The paper gives no
        // values; these were picked for the WaveApp tank (h = 0.5 m) and are not from the paper.
        float trappedAirMin = 1.0f;     // m/s, scaled velocity difference (eq.2)
        float trappedAirMax = 8.0f;
        float waveCrestMin = 0.1f;      // curvature (eq.5/6, dimensionless sum)
        float waveCrestMax = 1.0f;
        float energyMin = 0.1f;         // J/kg (kinetic energy per unit mass; the paper uses 0.5 m v^2)
        float energyMax = 2.0f;
        float trappedAirRate = 40.0f;   // k_ta, max particles per second per fluid particle (eq.8)
        float waveCrestRate = 40.0f;    // k_wc
        float lifetimeMin = 2.0f;       // s, scaled by the generation potential
        float lifetimeMax = 5.0f;
        float buoyancy = 0.5f;          // k_b (bubbles)
        float drag = 0.5f;              // k_d (bubbles); 1 = follow the fluid at once
        float rockImpactScale = 2.0f;   // trapped-air boost per m/s of impact speed on a rock (not from the paper)
        uint32_t sprayMaxNeighbours = 6;   // fewer fluid neighbours -> spray
        uint32_t bubbleMinNeighbours = 20; // more -> bubble, otherwise foam
        uint32_t maxParticles = 60000;
        uint32_t seed = 7;
    };

    struct FlipDiffuseStats
    {
        uint32_t spray = 0;
        uint32_t foam = 0;
        uint32_t bubble = 0;
        uint32_t spawned = 0;          // in the last Step
        uint32_t removed = 0;          // in the last Step (foam lifetime out, or the cap)
        uint32_t surfaceParticles = 0; // fluid particles whose potentials were evaluated
        float maxTrappedAir = 0.0f;    // raw v_diff
        float maxCurvature = 0.0f;     // raw kappa~ * delta_vn
        float maxEnergy = 0.0f;        // J/kg
        float stepMs = 0.0f;
    };

    // Spray, foam and air bubbles as a post-process of the FLIP water (Ihmsen, Akinci, Akinci &
    // Teschner 2012, "Unified Spray, Foam and Air Bubbles for Particle-Based Fluids"). Each step the
    // fluid particles get a trapped-air (eq.2), wave-crest (eq.5-7) and kinetic-energy potential,
    // clamped by eq.1, and emit n_d = I_k (k_ta I_ta + k_wc I_wc) dt diffuse particles (eq.8) in a
    // cylinder along their velocity. Diffuse particles are classified by their fluid-neighbour count
    // and advected without forces between them and without feedback on the water.
    // Deviations: potentials are evaluated only for fluid particles in cells next to Air (6-
    // neighbour), neighbours come from a cell hash with h = cellSize, the neighbour count of a
    // diffuse particle is the trilinear particle density per cell times 4/3 pi (not a radius query),
    // and the fluid velocity v~_f is the MAC grid velocity, not an SPH interpolation.
    class FlipDiffuseParticles
    {
    public:
        void Reset();
        // After FlipWaterSolver::Step(dt).
        void Step(const FlipWaterSolver& water, float dt);

        FlipDiffuseSettings& MutableSettings() { return m_settings; }
        const FlipDiffuseSettings& Settings() const { return m_settings; }
        const FlipDiffuseStats& Stats() const { return m_stats; }

        uint32_t Count() const { return static_cast<uint32_t>(m_kind.size()); }
        const std::vector<float>& Positions() const { return m_x; }   // world, 3 per particle
        const std::vector<float>& Velocities() const { return m_v; }
        const std::vector<uint8_t>& Kinds() const { return m_kind; }  // DiffuseKind
        const std::vector<float>& Lifetimes() const { return m_life; }

    private:
        void BuildFluidHash(const FlipWaterSolver& water);
        void BuildDensity(const FlipWaterSolver& water);
        void Emit(const FlipWaterSolver& water, float dt);
        void Advance(const FlipWaterSolver& water, float dt);
        float NeighbourCount(const FlipWaterSolver& water, const float x[3]) const;

        FlipDiffuseSettings m_settings;
        FlipDiffuseStats m_stats;
        uint32_t m_rng = 7;
        bool m_seeded = false;

        std::vector<float> m_x;
        std::vector<float> m_v;
        std::vector<float> m_life;
        std::vector<uint8_t> m_kind;

        // Fluid particles sorted by cell (counting sort), m_cellStart has CellCount() + 1 entries.
        std::vector<uint32_t> m_cellStart;
        std::vector<uint32_t> m_sorted;
        std::vector<float> m_density;   // fluid particles per cell
        std::vector<float> m_normal;    // per fluid particle (3), only for surface particles
        std::vector<uint8_t> m_surface; // per fluid particle
    };

    // wave.diffuse selftest: still water spawns nothing, a dam break spawns, stays in the tank and
    // two identical runs agree.
    uint32_t RunFlipDiffuseSelfTest(std::vector<std::string>* failures, uint32_t* total);
}
