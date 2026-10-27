#pragma once

#include "Renderer/Voxel/VoxelRegion.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Carries a solved pressure field from one streamed window to the next (6b stage 3b warm start).
    // oldP is indexed over oldRegion, outP is resized to newRegion; both use x + dimX * (y + dimY * z)
    // in their own region. Cells inside both regions (same anchor-grid voxel) keep their value, new
    // cells start at 0. Returns the number of carried cells.
    uint64_t ShiftWindowPressure(const std::vector<float>& oldP, const VoxelRegion& oldRegion,
                                 const VoxelRegion& newRegion, std::vector<float>& outP);
}

namespace SasamiEngine { using namespace Render; }
