#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // CPU-only checks of BuildSignedDistanceField (VoxelDistanceTransform.h) against a brute
    // force over every voxel pair on small grids: random occupancy at several densities and odd
    // dims, a single occupied voxel, an all-empty and an all-occupied grid. The comparison is
    // exact (both sides take sqrt of the same integer). Returns "OK ..." or "ERR ...".
    std::string RunVoxelSdfSelfTest();
}

namespace SasamiEngine { using namespace Render; }
