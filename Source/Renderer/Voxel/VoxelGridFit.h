#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"

#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    // Places a grid of cubic voxels over a world AABB. With E the longest box extent,
    // voxelSize = E / (maxDim - 1), and the box is grown by half a voxel on every side so
    // geometry lying exactly on the AABB faces is not clipped; the longest axis then holds
    // exactly maxDim voxels and the others ceil(extent / voxelSize + 1) (at least 1). Voxels stay
    // cubic and a flat scene does not pay for a full cube.
    // Returns a desc with zero dimensions when maxDim < 2 or the box is inverted/non-finite.
    VoxelGridDesc FitVoxelGridToBounds(const float boundsMin[3],
                                       const float boundsMax[3],
                                       uint32_t maxDim);
}

namespace SasamiEngine { using namespace Render; }
