#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // Checks VoxelizeSolidParity against an analytic inside test on fixed closed meshes (convex
    // boxes, rotated and off-grid, and a box with an inner cavity). CPU only. Returns a one-line
    // report starting with "OK" when every case matches voxel for voxel, "ERR" otherwise.
    std::string RunSolidVoxelizerSelfTest();
}

namespace SasamiEngine { using namespace Render; }
