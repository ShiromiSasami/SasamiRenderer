#pragma once

#include "FlipPressureSystem.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct QuadtreeColumnLayout;

    // One tall cell per column at most (restricted tall cell grid, Chentanez & Mueller 2011; tall cells
    // after Irving, Guendelman, Losasso & Fedkiw 2006). Cells [bottom, top) of the column are merged.
    struct TallCellColumn
    {
        uint16_t bottom = 0;
        uint16_t top = 0;   // top - bottom < 2 means the column has no tall cell
    };

    struct TallCellLayout
    {
        uint32_t nx = 0;
        uint32_t nz = 0;
        std::vector<TallCellColumn> columns;   // nx * nz, x fastest
        uint32_t tallColumns = 0;              // columns with a tall cell
        uint32_t mergedCells = 0;              // uniform cells covered by tall cells

        const TallCellColumn& At(uint32_t x, uint32_t z) const { return columns[x + size_t(nx) * z]; }
    };

    // Per column: bottom = lowest non-Solid cell; it must be Fluid, else no tall cell. topFluid = the
    // highest y such that every cell of [bottom, topFluid] is Fluid. top = topFluid + 1 - bandCells
    // (so bandCells uniform Fluid cells stay below the free surface). A tall cell exists only when
    // top - bottom >= minTallCells (and minTallCells is treated as at least 2). columnBands (nx * nz, from
    // ComputeOpticalLayerBands) replaces bandCells per column when its size matches.
    TallCellLayout BuildTallCellLayout(const FlipCellGrid& cells, uint32_t bandCells, uint32_t minTallCells,
                                       const std::vector<uint16_t>* columnBands = nullptr);

    // Solves the same system as SolveUniformPressure in a reduced basis: p = W q. A column's tall cell
    // has two unknowns, its bottom and top cell pressures, and the cells between them take the linear
    // interpolation in y (Irving et al. 2006 sec.3: two pressure samples per tall cell, linear in
    // between). Every other Fluid cell keeps its own unknown. The reduced system is the Galerkin form
    // W^T A W q = W^T (-divergence) ("sums the uniform finite-volume discretisation over the tall cell
    // with linear basis functions", Irving sec.5.2), assembled in CSR and solved with IC(0)-PCG.
    // Writes p = W q into `pressure` for every cell (0 for non-Fluid). stats.unknowns = reduced count.
    // Inside tall cells (and coarse quadtree leaves) the per-uniform-cell divergence is NOT driven to zero,
    // only its W-weighted sums.
    PressureSolveStats SolveTallCellPressure(const FlipCellGrid& cells, const TallCellLayout& layout,
                                             const std::vector<float>& divergence, std::vector<float>& pressure,
                                             const PressureSolveSettings& settings,
                                             const QuadtreeColumnLayout* quadtree = nullptr);

    // With a quadtree, the columns of one leaf share their unknowns (Narita et al. 2025 adapt the grid
    // horizontally; here a coarse leaf is a piecewise-constant aggregate, not their T-junction
    // interpolation). A leaf's columns must then share one tall range: this keeps the intersection of
    // their ranges, and drops the leaf's tall cell when a column has none or the intersection is shorter
    // than max(minTallCells, 2). tallColumns and mergedCells are recounted.
    void RestrictTallCellsToLeaves(TallCellLayout& layout, const QuadtreeColumnLayout& quadtree, uint32_t minTallCells);
}
