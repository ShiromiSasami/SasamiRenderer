#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/WindVelocityField.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class OccupancyBitGrid;

    // Fire spreading over the surface voxels of an occupancy grid, as a stochastic cellular automaton
    // after Alexandridis et al. 2008 ("A cellular automata model for forest fire spread prediction:
    // the case of the wildfire that swept through Spetses Island in 1990", Applied Mathematics and
    // Computation 204(1), doi:10.1016/j.amc.2008.06.046). A burning cell ignites each unburnt surface
    // cell among its 26 neighbours with probability
    //   p = p0 * exp(c1 * V) * exp(V * c2 * (cos(theta) - 1))           (clamped to 1)
    // once, on the first tick after it caught fire (the paper's burning state lasts one step), where V is the local wind speed (m/s) and theta the angle between the wind and the
    // direction to the neighbour. The paper's vegetation/density factors are 1 here and its slope
    // factor exp(a * slope_deg) is omitted (a = 0.078 per degree makes vertical neighbours, slope 90
    // deg, always burn); c1, c2, p0 are the paper's values as quoted by later work (the original text
    // was not checked). The wind is sampled one voxel outside the burning cell along its outward
    // normal, because the field is zero inside solids.
    //
    // A surface cell is an occupied voxel with at least one of its 6 face neighbours empty (or
    // outside the grid). States: Unburnt -> Burning (for burnTime seconds) -> Burnt. burnTime only sets
    // how long a cell glows; it does not add spread attempts (retrying every tick saturated the spread in
    // every direction, so upwind and downwind fronts ran equally fast).
    struct WindFireSettings
    {
        float p0 = 0.58f;
        float c1 = 0.045f;          // s/m
        float c2 = 0.131f;          // s/m
        float tickInterval = 0.25f; // seconds of sim time per CA step
        float burnTime = 6.0f;      // seconds a cell burns
        // Cells farther than this (voxels, Euclidean between cell centres) from every Ignite target never
        // ignite; 0 = unlimited. Bounds the burning/burnt sets, which the overlay draws and occlusion-tests.
        float maxSpreadRadius = 0.0f;
    };

    enum class FireCellState : uint8_t
    {
        None,       // not a surface cell
        Unburnt,
        Burning,
        Burnt,
    };

    struct WindFireStats
    {
        uint32_t surfaceCells = 0;
        uint32_t burning = 0;
        uint32_t burnt = 0;
        uint64_t ticks = 0;
        uint64_t ignitions = 0;     // spread events (Ignite() calls not counted)
    };

    class WindFireSpread
    {
    public:
        // Finds the surface cells of `grid` and their outward normals; every cell starts Unburnt.
        void Build(const OccupancyBitGrid& grid, const VoxelGridDesc& desc, const WindFireSettings& settings, uint32_t seed);
        bool IsBuilt() const { return !m_state.empty(); }
        void SetSettings(const WindFireSettings& settings) { m_settings = settings; }
        const WindFireSettings& Settings() const { return m_settings; }

        // Sets the surface cell nearest to `worldPosition` (within `searchRadius` voxels, Chebyshev)
        // Burning. Returns false when there is none.
        bool Ignite(const float worldPosition[3], uint32_t searchRadius = 4);
        // Back to all Unburnt.
        void Extinguish();

        // Advances sim time by dt; runs one CA tick per elapsed tickInterval (carry kept).
        void Step(const IWindVelocityField& wind, float dt);

        // Linear voxel indices (x + y*dimX + z*dimX*dimY) of the burning cells, in no particular order.
        const std::vector<uint32_t>& BurningCells() const { return m_burning; }
        // Every Burnt cell.
        const std::vector<uint32_t>& BurntCells() const { return m_burnt; }
        FireCellState State(uint32_t linear) const { return static_cast<FireCellState>(m_state[linear]); }
        // Remaining burn time / burnTime of a burning cell, 0 otherwise.
        float Remaining01(uint32_t linear) const;
        // World position of the cell's centre pushed half a voxel along its outward normal (on the face).
        void SurfacePoint(uint32_t linear, float outPosition[3]) const;
        // Unit outward normal (zero for a non-surface cell).
        void OutwardNormal(uint32_t linear, float outNormal[3]) const;
        const VoxelGridDesc& Desc() const { return m_desc; }
        const WindFireStats& Stats() const { return m_stats; }

    private:
        void Tick(const IWindVelocityField& wind);
        bool WithinSpreadRadius(int x, int y, int z) const;
        float Random01();

        VoxelGridDesc m_desc;
        WindFireSettings m_settings;
        WindFireStats m_stats;
        uint32_t m_rng = 1;
        float m_tickCarry = 0.0f;
        std::vector<uint8_t> m_state;           // FireCellState per voxel
        std::vector<int8_t> m_normal;           // 3 per voxel, outward normal * 127
        std::vector<float> m_remaining;         // seconds, per voxel (only meaningful while Burning)
        std::vector<uint32_t> m_burning;
        std::vector<uint32_t> m_front;          // ignited since the last tick; each spreads once
        std::vector<uint32_t> m_burnt;
        std::vector<int> m_igniteCells;         // 3 per Ignite target cell, for maxSpreadRadius
    };
}
