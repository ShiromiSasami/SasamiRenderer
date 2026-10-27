#pragma once

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    // Grid placement in world space. Voxel (x, y, z) covers
    // [origin + (x, y, z) * voxelSize, origin + (x + 1, y + 1, z + 1) * voxelSize].
    struct VoxelGridDesc
    {
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float voxelSize = 1.0f;
        uint32_t dimX = 0;
        uint32_t dimY = 0;
        uint32_t dimZ = 0;
    };

    // Triangle soup input. positions holds xyz triples; indices holds 3 per triangle.
    // When indices is null, every 3 consecutive positions form one triangle.
    struct VoxelizerMeshInput
    {
        const float* positions = nullptr;
        uint32_t vertexCount = 0;
        uint32_t positionStrideInFloats = 3;
        const uint32_t* indices = nullptr;
        uint32_t indexCount = 0;
    };

    struct VoxelizeStats
    {
        uint64_t trianglesTested = 0;
        uint64_t trianglesSkipped = 0;   // out-of-range indices or fully outside the grid
        uint64_t boxTests = 0;           // SAT invocations (cost metric)
        uint64_t occupiedVoxels = 0;     // after this call, whole grid
    };

    // Reference surface voxelizer: marks every voxel whose box overlaps a triangle
    // (conservative 26-separating surface voxelization, Schwarz & Seidel 2010 sec. 3).
    // It is the CPU ground truth for the GPU voxelizer, not a runtime path.
    // Bits are OR-ed into `grid`, which must already be sized to desc's dimensions;
    // call grid.Clear() first for a fresh voxelization.
    VoxelizeStats VoxelizeSurface(const VoxelGridDesc& desc,
                                  const VoxelizerMeshInput& mesh,
                                  OccupancyBitGrid& grid);

    struct VoxelRegion;
    // VoxelizeSurface restricted to `region` (VoxelRegion.h): `regionGrid` must already be sized to
    // region.size and receives voxel (region.min + local) at local. Voxel boxes use desc's full-grid
    // placement, so every bit equals the full-grid result at that voxel. VoxelizeSurface is this
    // with FullVoxelRegion(desc). An empty or out-of-grid region, or a size mismatch, changes nothing.
    VoxelizeStats VoxelizeSurfaceRegion(const VoxelGridDesc& desc,
                                        const VoxelRegion& region,
                                        const VoxelizerMeshInput& mesh,
                                        OccupancyBitGrid& regionGrid);
}

namespace SasamiEngine { using namespace Render; }
