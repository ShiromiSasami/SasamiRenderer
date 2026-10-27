#pragma once

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    // Counts the voxel rows along +X whose last voxel (x = dimX - 1) is set in a parity grid
    // written by VoxelizeSolidParity / GpuSolidVoxelizer (interior only, before the surface is
    // OR-ed in). FitVoxelGridToBounds puts the last voxel centre at or past the geometry's
    // +X bound, so for a closed mesh every row has crossed an even number of surfaces there;
    // an odd count means the row went through a hole and its "inside" streak runs to the end
    // of the grid (the leak Nooruddin & Turk 2003 describe for non-watertight meshes).
    //
    // An upper bound: a row whose last crossing lies exactly on the last voxel centre is not
    // flipped there either, and is counted although the mesh may be closed.
    uint64_t CountParityLeakRows(const OccupancyBitGrid& interior);
}

namespace SasamiEngine { using namespace Render; }
