#pragma once

#include "FlipPressureSystem.h"

#include "Renderer/Voxel/StaggeredVelocityGrid.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct OpticalLayerSettings
    {
        bool enabled = false;
        float scale = 64.0f;            // a in d = clamp(a * I, minCells, maxCells)
        float referenceSpeed = 1.0f;    // m/s, divides the local cost (makes it dimensionless)
        uint32_t minCells = 2;
        uint32_t maxCells = 6;
        uint32_t smoothIterations = 2;  // passes of the 3x3 tall-top smoothing
    };

    struct OpticalLayerStats
    {
        uint32_t minBand = 0;          // over grounded wet columns, after dilation and smoothing
        uint32_t maxBand = 0;
        float meanBand = 0.0f;
        float maxIndicator = 0.0f;     // max over columns of I
    };

    // Adaptive optical layers after Narita & Kanai (PG2026 "Beyond Horizontal Refinement", eq.2-6; the
    // AOL paper itself, CGF 45.2 e70357, was not available). Returns, per column (nx * nz, x fastest),
    // the number of uniform Fluid cells to keep below the surface of its grounded run (the band passed
    // to BuildTallCellLayout), 0 for columns without one.
    //   I(i,k) = (1/ny) * sum over the grounded Fluid run of q_ijk, d = clamp(round(a I), min, max).
    //   The surface cell of each column dilates over Manhattan distance |dx|+|dz|+|dy| < d (paper (e)),
    //   then the tall-cell top height field is box-smoothed (paper (f), kernel not given there).
    // Choices of this project (the paper's q is not known): q = |u_h(j+1) - 2 u_h(j) + u_h(j-1)| / vRef,
    // the second y-difference of the cell-centred horizontal velocity, i.e. its departure from the linear
    // profile a tall cell assumes; 0 at the ends of the run. Smoothing only lowers a top (min with the
    // 3x3 mean of wet neighbours), so no column ends with a thinner layer than its dilated d. The paper's
    // d_min = max(2w, 4) for quadtree width w and d_max = ny/8 are replaced by minCells/maxCells, because
    // ny/8 = 2 on the 16-cell tank. Disconnected blobs are not handled (they never become tall cells).
    std::vector<uint16_t> ComputeOpticalLayerBands(const FlipCellGrid& cells,
                                                   const Render::Voxel::StaggeredVelocityGrid& velocity,
                                                   const OpticalLayerSettings& settings, OpticalLayerStats* stats);
}
