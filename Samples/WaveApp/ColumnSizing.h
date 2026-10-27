#pragma once

#include "FlipPressureSystem.h"

#include "Renderer/Voxel/StaggeredVelocityGrid.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct ColumnSizingSettings
    {
        uint32_t maxLog2 = 3;
        float velocityThreshold = 0.5f;    // m/s: |v_surface - v_neighbour| over the 4 neighbour columns
        float curvatureThreshold = 0.5f;   // cells: |sum of 4 neighbour heights - 4 h|
        bool refineWetDryEdge = true;      // wet column next to a dry one
        bool refineSolid = true;           // a Fluid cell with an in-grid Solid cell beside it (x or z)
    };

    struct ColumnSizingStats
    {
        uint32_t markedColumns = 0;
    };

    // Sizing function of Quadtree Tall Cells (Narita et al. 2025 sec.4), flattened per column: a column is
    // marked when one of the criteria above holds, and its desired level is the largest L with
    // 2^L <= max(1, d), d = Chebyshev distance (in columns) to the nearest marked column, capped at
    // maxLog2 (maxLog2 everywhere when nothing is marked).
    // Column height h = (highest Fluid y) + 1 in cells, 0 for a column with no Fluid cell (dry).
    // Surface velocity = cell-centred velocity (face averages) of that highest Fluid cell.
    // Deviation from the paper: it is evaluated on the current step's cells and velocities (after P2G
    // and body forces), not on the previous step's, and the criteria/thresholds are this project's.
    std::vector<uint8_t> ComputeColumnDesiredLog2(const FlipCellGrid& cells,
                                                  const Render::Voxel::StaggeredVelocityGrid& velocity,
                                                  const ColumnSizingSettings& settings, ColumnSizingStats* stats);
}
