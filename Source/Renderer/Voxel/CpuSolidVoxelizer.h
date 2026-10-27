#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    struct SolidVoxelizeStats
    {
        uint64_t trianglesTested = 0;
        uint64_t trianglesSkipped = 0;   // out-of-range indices, edge-on in YZ, or outside the grid
        uint64_t rowFlips = 0;           // (triangle, voxel row) crossings that toggled a row tail
        uint64_t occupiedVoxels = 0;     // after this call, whole grid
    };

    // Reference solid voxelizer by crossing parity (Schwarz & Seidel 2010, sec. 4; the same
    // parity-count idea as Nooruddin & Turk 2003). Each voxel row along +X is a ray through the
    // voxel centres (y, z). For every triangle whose YZ projection covers a row's centre, the
    // row's bits from the first voxel whose centre lies at x greater than the crossing to the
    // end of the row are flipped. After all triangles a bit is 1 when an odd number of surfaces
    // lies at smaller x than the voxel centre, i.e. the centre is inside a closed mesh.
    //
    // Coverage of a centre that lies exactly on a projected edge is decided per edge with the
    // edge endpoints in a canonical (lexicographic) order, so two triangles sharing that edge
    // evaluate the same float expression and exactly one of them claims the centre. A centre
    // that coincides with a projected vertex is not given this treatment and may be counted
    // zero or several times (rare with arbitrary float coordinates).
    //
    // GpuSolidVoxelizer must stay bit-identical to this function, so every decision is an
    // exact comparison of the same double expressions: coverage and the crossing test are
    // evaluated in double from float inputs, the first voxel past the crossing is found by a
    // binary search on that test rather than by a division, and the candidate row range is a
    // superset of the triangle's YZ bounds (one extra row each side) whose rows are then
    // filtered by the exact coverage test.
    //
    // The result only depends on crossing parity, so winding order does not matter, but a mesh
    // with holes leaks: a row through a hole keeps an odd count up to the grid's end. Surface
    // voxels are not added; OR the result with VoxelizeSurface for a conservative solid.
    // Bits are XOR-ed into `grid`, which must already be sized to desc's dimensions; call
    // grid.Clear() first for a fresh voxelization.
    SolidVoxelizeStats VoxelizeSolidParity(const VoxelGridDesc& desc,
                                           const VoxelizerMeshInput& mesh,
                                           OccupancyBitGrid& grid);
}

namespace SasamiEngine { using namespace Render; }
