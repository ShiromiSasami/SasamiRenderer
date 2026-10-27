#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // CPU-only checks of ExtractSdfIsoSurface on synthetic occupancy: a voxelized sphere (closed mesh,
    // outward normals, volume against the voxel count and the analytic sphere, vertices within half a
    // voxel of the grid), a box touching the grid border (capped, closed), a single voxel, and the empty /
    // mismatched cases. Returns "OK checks=N ..." or "ERR ...".
    std::string RunSdfIsoSurfaceSelfTest();
}

namespace SasamiEngine { using namespace Render; }
