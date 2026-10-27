#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    enum class FlipCell : uint8_t
    {
        Air = 0,
        Fluid = 1,
        Solid = 2,
    };

    // Cell classification of the FLIP water grid, nx * ny * nz cells, x fastest, then y, then z
    // (the StaggeredVelocityGrid order). Cells outside the grid count as Solid, except above the top
    // (y >= ny), which is open Air.
    struct FlipCellGrid
    {
        uint32_t nx = 0;
        uint32_t ny = 0;
        uint32_t nz = 0;
        std::vector<uint8_t> type;   // FlipCell per cell

        size_t Index(uint32_t x, uint32_t y, uint32_t z) const { return x + size_t(nx) * (y + size_t(ny) * z); }
        FlipCell At(uint32_t x, uint32_t y, uint32_t z) const { return static_cast<FlipCell>(type[Index(x, y, z)]); }
        size_t CellCount() const { return size_t(nx) * ny * nz; }
    };

    struct PressureSolveSettings
    {
        uint32_t maxIterations = 200;
        float tolerance = 1e-5f;   // stop when max|r| <= tolerance * max|b|
    };

    struct PressureSolveStats
    {
        uint32_t unknowns = 0;
        uint32_t iterations = 0;
        float residual = 0.0f;     // max|r| / max|b| at exit (0 when b == 0)
        bool converged = false;
        float buildMs = 0.0f;      // matrix + preconditioner
        float solveMs = 0.0f;      // PCG loop
    };

    // Free-surface pressure Poisson system on the uniform grid (Bridson & Mueller-Fischer 2007 ch.4,
    // Foster & Fedkiw 2001): unknowns are the Fluid cells, Air cells are Dirichlet p = 0, Solid cells
    // are Neumann (their faces keep the solid velocity and are not updated). Pressure is scaled by
    // dt / (rho dx), so it has units of m/s and the velocity update on a face between cells a (lower
    // index along the axis) and b is u -= p_b - p_a, applied when neither side is Solid.
    //   (A p)_c = sum over non-Solid neighbours n of (p_c - p_n)   (p_n = 0 for Air)
    // and the system solved is A p = -divergence, where divergence[c] is the net outflow of cell c
    // (StaggeredVelocityGrid::Divergence, m/s). `divergence` and `pressure` are CellCount() long;
    // `pressure` is resized and every non-Fluid entry is written as 0.
    // Preconditioner: MIC(0), tau 0.97, sigma 0.25 (as PressureProjector). Serial.
    PressureSolveStats SolveUniformPressure(const FlipCellGrid& cells, const std::vector<float>& divergence,
                                            std::vector<float>& pressure, const PressureSolveSettings& settings);
}
