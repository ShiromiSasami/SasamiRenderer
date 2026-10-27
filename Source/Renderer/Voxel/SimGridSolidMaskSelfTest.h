#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // Checks BuildSimGridSolidMask on synthetic cases: aligned same-resolution (bit-exact copy),
    // coarser and finer sim grids (against an independent brute-force overlap test), a disjoint
    // grid, and a single voxel tiled exactly by sim cells. CPU only. Returns a report starting
    // with "OK " when every case passes ("name:solid=N" per case), "ERR " otherwise, naming the
    // failing case and its first mismatching cell.
    std::string RunSimGridSolidMaskSelfTest();
}

namespace SasamiEngine { using namespace Render; }
