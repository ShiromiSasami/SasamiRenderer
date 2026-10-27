#pragma once

#include "FlipPressureSystem.h"
#include "ColumnSizing.h"
#include "OpticalLayerSizing.h"
#include "QuadtreeColumnLayout.h"
#include "TallCellPressureBasis.h"
#include "Renderer/Voxel/StaggeredVelocityGrid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    using SeaBedHeightFunction = float (*)(float x, float z);

    // A rock on the sea bed: the upper half of an ellipsoid, extruded straight down to the bed
    // (the side below the equator is a vertical wall), so the bed stays a height field.
    struct FlipRock
    {
        float center[3] = { 0.0f, 0.0f, 0.0f };   // world
        float radius[3] = { 1.0f, 1.0f, 1.0f };   // m, half extents
    };

    // World y of the rock top at (x, z), -infinity outside its footprint.
    float RockTopHeight(const FlipRock& rock, float x, float z);

    struct FlipWaterSettings
    {
        uint32_t nx = 64;
        uint32_t ny = 16;
        uint32_t nz = 24;
        float cellSize = 0.5f;                       // m
        float origin[3] = { -12.0f, -6.0f, -6.0f };  // world position of grid corner (0,0,0)
        float waterLevel = 0.0f;                     // initial still-water height (world y)
        float gravity = 9.81f;
        float flipRatio = 0.95f;                     // Zhu & Bridson 2005: v = ratio*FLIP + (1-ratio)*PIC
        uint32_t particlesPerAxis = 2;               // per cell, jittered
        float maxCfl = 1.0f;                         // max cells travelled per substep
        uint32_t maxSubsteps = 4;
        float paddleAmplitude = 0.0f;                // m/s, piston wave maker on the x = 0 wall: u = A cos(w t)
        float paddlePeriod = 4.0f;                   // s
        bool paddleExternal = false;                 // the caller drives the wall with SetPaddleDrive (cosine ignored)
        bool tallCells = false;                      // pressure in the tall-cell basis (Irving et al. 2006)
        uint32_t bandCells = 3;                      // uniform fluid cells kept below the surface
        uint32_t minTallCells = 3;
        OpticalLayerSettings opticalLayers;          // per-column band (Narita & Kanai 2026), tall cells only
        bool quadtree = false;                       // horizontal quadtree of columns (Narita et al. 2025)
        ColumnSizingSettings sizing;                 // maxLog2 = coarsest leaf, 2^maxLog2 columns wide
        PressureSolveSettings pressure;
        std::vector<FlipRock> rocks;                 // merged into the sea bed with max()
        uint32_t seed = 1;
        bool parallel = true;                        // ParallelFor over particles / z slices
    };

    struct FlipWaterStats
    {
        uint32_t particles = 0;
        uint32_t fluidCells = 0;
        uint32_t solidCells = 0;
        uint32_t unknowns = 0;
        uint32_t iterations = 0;       // summed over substeps of the last Step
        uint32_t substeps = 0;
        uint32_t tallColumns = 0;
        uint32_t mergedCells = 0;
        uint32_t quadLeaves = 0;       // 0 without the quadtree
        uint32_t quadFineColumns = 0;  // columns in level-0 leaves
        uint32_t opticalMinBand = 0;   // optical-layer band over grounded wet columns (0 when off)
        uint32_t opticalMaxBand = 0;
        float opticalMeanBand = 0.0f;
        float opticalMaxIndicator = 0.0f;
        uint32_t quadMarked = 0;       // columns marked by the sizing criteria
        float residual = 0.0f;         // worst substep
        bool converged = true;         // every substep
        float p2gMs = 0.0f;
        float solveMs = 0.0f;          // build + solve
        float g2pMs = 0.0f;            // G2P + advection
        float stepMs = 0.0f;
        float maxDivergence = 0.0f;    // max |div| over fluid cells outside tall cells and coarse leaves (m/s)
        float maxDivergenceCoarse = 0.0f;// same, over cells of coarse quadtree leaves outside tall cells
        float maxDivergenceTall = 0.0f;// same, over cells inside tall cells
        float maxSpeed = 0.0f;         // particles, m/s
        float surfaceMin = 0.0f;       // world y of the column heights over wet columns
        float surfaceMax = 0.0f;
        double time = 0.0;             // simulated seconds since Reset
    };

    // FLIP/PIC water (Zhu & Bridson 2005) in a box over a height-field sea bed, on a MAC grid with a free
    // surface (cells with particles are Fluid, others Air; cells whose centre is below the sea bed and
    // the side/bottom walls are Solid; the top is open). The pressure solve is either the uniform
    // MIC(0)-PCG or the tall-cell reduced basis (Irving, Guendelman, Losasso & Fedkiw 2006): only the
    // pressure unknowns are reduced, velocities stay on the uniform MAC grid (deviation from the paper,
    // which also stores one vertical velocity per tall cell).
    class FlipWaterSolver
    {
    public:
        // Fills every non-solid cell whose centre is below waterLevel with particlesPerAxis^3 jittered
        // particles at rest. seaBed may be null (flat bottom at the grid floor).
        void Reset(const FlipWaterSettings& settings, SeaBedHeightFunction seaBed);
        // Advances by dt seconds in ceil(dt / cflDt) substeps, at most maxSubsteps (the last one may
        // then exceed maxCfl).
        void Step(float dt);
        // Removes every particle and adds a block of water [x0,x1] x [y0,y1] x [z0,z1] (world) instead
        // (used by the dam-break test).
        void ResetToBlock(const float boxMin[3], const float boxMax[3]);

        // External wave maker (paddleExternal): the x = 0 wall face at height y moves with
        // surfaceVelocity * exp(decayPerMeter * min(y - waterLevel, 0)), the deep-water decay of an
        // orbital velocity (Airy theory). Kept until the next call; Reset clears it.
        void SetPaddleDrive(float surfaceVelocity, float decayPerMeter);
        float PaddleDriveVelocity() const { return m_paddleDriveU; }

        const FlipWaterSettings& Settings() const { return m_settings; }
        FlipWaterSettings& MutableSettings() { return m_settings; }
        const FlipWaterStats& Stats() const { return m_stats; }

        uint32_t ParticleCount() const { return static_cast<uint32_t>(m_px.size() / 3); }
        const std::vector<float>& ParticlePositions() const { return m_px; }   // world, 3 per particle
        const std::vector<float>& ParticleVelocities() const { return m_pv; }

        // Water surface per grid column (nx * nz, x fastest, world y): the highest particle in the
        // column plus half the particle spacing, 3x3 box-smoothed over wet columns; dry columns hold
        // the sea-bed height (or the grid floor). Updated at the end of each Step and Reset.
        const std::vector<float>& ColumnHeights() const { return m_columnHeight; }
        const std::vector<uint8_t>& ColumnWet() const { return m_columnWet; }
        // Bilinear between column centres, clamped to the grid; world xz.
        float SampleSurfaceHeight(float x, float z) const;
        SeaBedHeightFunction SeaBed() const { return m_seaBed; }
        // Sea bed including the rocks (what the solver treats as solid below).
        float BedHeight(float x, float z) const;
        // Pushes a point that left the box or went under the sea bed back inside (1e-3 cell from the
        // wall) and zeroes the velocity components pointing into the wall; returns true when it moved
        // it. Same rule as the particle Advect, for passengers such as the diffuse particles.
        bool ConfineToDomain(float position[3], float velocity[3]) const;

        const Render::Voxel::StaggeredVelocityGrid& Velocity() const { return m_velocity; }
        const FlipCellGrid& Cells() const { return m_cells; }
        const TallCellLayout& Layout() const { return m_layout; }
        const QuadtreeColumnLayout& Quadtree() const { return m_quadtree; }   // empty without the quadtree

    private:
        void Substep(float dt);
        void ClassifyCells();
        void TransferToGrid();
        void ApplyBoundaryVelocities();
        void Project();
        void Extrapolate();
        void TransferToParticles();
        void Advect(float dt);
        void UpdateColumnHeights();
        float SeaBed(float x, float z) const;

        FlipWaterSettings m_settings;
        SeaBedHeightFunction m_seaBed = nullptr;
        float m_paddleDriveU = 0.0f;
        float m_paddleDriveK = 0.0f;
        FlipWaterStats m_stats;
        std::vector<float> m_px;
        std::vector<float> m_pv;
        Render::Voxel::StaggeredVelocityGrid m_velocity;
        Render::Voxel::StaggeredVelocityGrid m_savedVelocity;   // after P2G, before forces (FLIP delta)
        std::vector<uint8_t> m_staticSolid;                     // sea bed + nothing else, per cell
        FlipCellGrid m_cells;
        TallCellLayout m_layout;
        QuadtreeColumnLayout m_quadtree;
        std::vector<float> m_divergence;
        std::vector<float> m_pressure;
        std::vector<float> m_columnHeight;
        std::vector<uint8_t> m_columnWet;
    };

    // Returns the number of checks that passed; failures get a description each.
    uint32_t RunFlipWaterSelfTest(std::vector<std::string>* failures, uint32_t* total);
}
