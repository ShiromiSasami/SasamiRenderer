#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"

#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    // A box of voxels inside a VoxelGridDesc: voxels min[a] .. min[a] + size[a] - 1 on each axis.
    // Region voxelization (VoxelizeSurfaceRegion, GpuSurfaceVoxelizer::VoxelizeRegion) still places
    // every voxel with the FULL grid's origin + index * voxelSize, so the bits it writes for a voxel
    // are exactly the ones a full-grid run writes there; only the output grid is region-sized.
    struct VoxelRegion
    {
        uint32_t min[3] = { 0, 0, 0 };
        uint32_t size[3] = { 0, 0, 0 };

        bool IsEmpty() const { return size[0] == 0 || size[1] == 0 || size[2] == 0; }
        uint64_t VoxelCount() const { return static_cast<uint64_t>(size[0]) * size[1] * size[2]; }
    };

    // The whole grid.
    VoxelRegion FullVoxelRegion(const VoxelGridDesc& desc);

    // Voxels whose box can touch the world AABB [boundsMin, boundsMax], widened by padVoxels on
    // every side and clamped to the grid. Returns false (and an empty region) when the widened box
    // misses the grid entirely.
    bool VoxelRegionFromWorldBounds(const VoxelGridDesc& desc,
                                    const float boundsMin[3],
                                    const float boundsMax[3],
                                    uint32_t padVoxels,
                                    VoxelRegion& outRegion);

    // Smallest region containing both (an empty operand is ignored).
    VoxelRegion UnionVoxelRegions(const VoxelRegion& a, const VoxelRegion& b);
    // True when the regions share at least one voxel.
    bool VoxelRegionsOverlap(const VoxelRegion& a, const VoxelRegion& b);

    // World-space box the region's voxels cover: origin + min * voxelSize .. origin + (min + size) * voxelSize.
    void VoxelRegionWorldBounds(const VoxelGridDesc& desc, const VoxelRegion& region,
                                float outMin[3], float outMax[3]);
}

namespace SasamiEngine { using namespace Render; }
