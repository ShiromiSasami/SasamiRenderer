#include "Renderer/Voxel/VoxelRegion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    VoxelRegion FullVoxelRegion(const VoxelGridDesc& desc)
    {
        VoxelRegion region;
        region.size[0] = desc.dimX;
        region.size[1] = desc.dimY;
        region.size[2] = desc.dimZ;
        return region;
    }

    bool VoxelRegionFromWorldBounds(const VoxelGridDesc& desc,
                                    const float boundsMin[3],
                                    const float boundsMax[3],
                                    uint32_t padVoxels,
                                    VoxelRegion& outRegion)
    {
        outRegion = {};
        if (desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0 || !(desc.voxelSize > 0.0f))
        {
            return false;
        }

        const uint32_t dims[3] = { desc.dimX, desc.dimY, desc.dimZ };
        int64_t lo[3];
        int64_t hi[3];
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double tMin = (static_cast<double>(boundsMin[axis]) - desc.origin[axis]) / desc.voxelSize;
            const double tMax = (static_cast<double>(boundsMax[axis]) - desc.origin[axis]) / desc.voxelSize;
            lo[axis] = static_cast<int64_t>(std::floor(tMin)) - static_cast<int64_t>(padVoxels);
            hi[axis] = static_cast<int64_t>(std::floor(tMax)) + static_cast<int64_t>(padVoxels);
            if (hi[axis] < 0 || lo[axis] > static_cast<int64_t>(dims[axis]) - 1)
            {
                return false;
            }
            lo[axis] = std::max<int64_t>(lo[axis], 0);
            hi[axis] = std::min<int64_t>(hi[axis], static_cast<int64_t>(dims[axis]) - 1);
        }

        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            outRegion.min[axis] = static_cast<uint32_t>(lo[axis]);
            outRegion.size[axis] = static_cast<uint32_t>(hi[axis] - lo[axis] + 1);
        }
        return true;
    }

    VoxelRegion UnionVoxelRegions(const VoxelRegion& a, const VoxelRegion& b)
    {
        if (a.IsEmpty())
        {
            return b;
        }
        if (b.IsEmpty())
        {
            return a;
        }

        VoxelRegion result;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const uint64_t endA = static_cast<uint64_t>(a.min[axis]) + a.size[axis];
            const uint64_t endB = static_cast<uint64_t>(b.min[axis]) + b.size[axis];
            result.min[axis] = std::min(a.min[axis], b.min[axis]);
            result.size[axis] = static_cast<uint32_t>(std::max(endA, endB) - result.min[axis]);
        }
        return result;
    }

    bool VoxelRegionsOverlap(const VoxelRegion& a, const VoxelRegion& b)
    {
        if (a.IsEmpty() || b.IsEmpty())
        {
            return false;
        }

        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const uint64_t endA = static_cast<uint64_t>(a.min[axis]) + a.size[axis];
            const uint64_t endB = static_cast<uint64_t>(b.min[axis]) + b.size[axis];
            if (a.min[axis] >= endB || b.min[axis] >= endA)
            {
                return false;
            }
        }
        return true;
    }

    void VoxelRegionWorldBounds(const VoxelGridDesc& desc, const VoxelRegion& region,
                                float outMin[3], float outMax[3])
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double origin = desc.origin[axis];
            outMin[axis] = static_cast<float>(origin + static_cast<double>(region.min[axis]) * desc.voxelSize);
            outMax[axis] = static_cast<float>(
                origin + (static_cast<double>(region.min[axis]) + region.size[axis]) * desc.voxelSize);
        }
    }
}
