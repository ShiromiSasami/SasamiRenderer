#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    // CPU reference for Position Based Fluids (Macklin & Mueller 2013, ACM TOG 32(4)).
    // Single threaded and deterministic: the same setup gives bit-identical results.
    struct PbfParams
    {
        float particleRadius = 0.025f;   // h = 4r puts a rest lattice at density ratio ~1 (r = 0.05 gives ~1.57)
        float smoothingRadius = 0.1f;
        float restDensity = 1000.0f;
        float particleMass = 1.0f;       // set by Reset from ComputeParticleMass
        float relaxationEpsilon = 600.0f; // CFM epsilon, eq. 11
        // The paper's k = 0.1 is in its own unit system. With SI mass (rho0 * (2r)^3) it makes s_corr ~1000x
        // larger than lambda, and a dam break collapses to density ratio ~160 (measured). 1e-3 stays at ~0.98.
        float scorrK = 0.001f;
        float scorrN = 4.0f;
        float scorrDeltaQ = 0.2f;         // fraction of smoothingRadius
        float xsphC = 0.01f;
        float gravityY = -9.81f;
        uint32_t solverIterations = 4;
        float boxMin[3] = { 0.0f, 0.0f, 0.0f };
        float boxMax[3] = { 2.0f, 2.0f, 2.0f };
    };

    struct PbfStepStats
    {
        float meanDensityRatio = 0.0f;
        float maxDensityRatio = 0.0f;
        uint32_t maxNeighbors = 0;
        double milliseconds = 0.0;
    };

    // Mass of one particle filling a (2 * particleRadius)^3 cell at rest density.
    inline float ComputeParticleMass(const PbfParams& params)
    {
        const float side = 2.0f * params.particleRadius;
        return params.restDensity * side * side * side;
    }

    // Poly6 kernel W(r, h) taking the squared distance; zero for r >= h.
    float Poly6Kernel(float r2, float h);

    // Reference neighbor search (|r| < h, self excluded) over xyz-packed positions.
    void FindNeighborsBruteForce(const std::vector<float>& positions, float h, std::vector<std::vector<uint32_t>>& out);

    class PbfSolver
    {
    public:
        // Clears all particles and adopts `params` (particleMass is recomputed).
        void Reset(const PbfParams& params);

        // Fills [min, max] with a lattice of spacing 2 * particleRadius (cell centers), appended to the particles.
        void SpawnBlock(const float (&min)[3], const float (&max)[3]);

        void Step(float dt);

        // Builds the uniform grid and neighbor lists for arbitrary positions (Step calls this on predicted positions).
        void BuildNeighbors(const std::vector<float>& positions);

        uint32_t ParticleCount() const { return static_cast<uint32_t>(m_positions.size() / 3); }
        const std::vector<float>& Positions() const { return m_positions; }
        const std::vector<float>& Velocities() const { return m_velocities; }
        const std::vector<std::vector<uint32_t>>& LastNeighbors() const { return m_neighbors; }
        const PbfStepStats& LastStats() const { return m_stats; }
        const PbfParams& Params() const { return m_params; }

    private:
        PbfParams m_params;
        PbfStepStats m_stats;

        std::vector<float> m_positions;
        std::vector<float> m_velocities;
        std::vector<float> m_predicted;
        std::vector<float> m_delta;
        std::vector<float> m_nextVelocities;
        std::vector<float> m_lambda;
        std::vector<float> m_density;

        std::vector<std::vector<uint32_t>> m_neighbors;

        uint32_t m_gridDim[3] = { 1, 1, 1 };
        std::vector<uint32_t> m_cellOf;
        std::vector<uint32_t> m_cellStart;
        std::vector<uint32_t> m_cellFill;
        std::vector<uint32_t> m_sortedIds;
    };
}
