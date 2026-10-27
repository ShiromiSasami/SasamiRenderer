#include "Renderer/Voxel/VoxelGridFit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    VoxelGridDesc FitVoxelGridToBounds(const float boundsMin[3],
                                       const float boundsMax[3],
                                       uint32_t maxDim)
    {
        VoxelGridDesc desc;
        desc.dimX = 0;
        desc.dimY = 0;
        desc.dimZ = 0;
        if (maxDim < 2)
        {
            return desc;
        }

        float extent[3];
        float longest = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(boundsMin[axis]) || !std::isfinite(boundsMax[axis]) || boundsMin[axis] > boundsMax[axis])
            {
                return desc;
            }
            extent[axis] = boundsMax[axis] - boundsMin[axis];
            longest = (std::max)(longest, extent[axis]);
        }

        const float voxelSize = longest > 0.0f ? longest / static_cast<float>(maxDim - 1) : 1.0f;
        desc.voxelSize = voxelSize;

        uint32_t dims[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            desc.origin[axis] = boundsMin[axis] - 0.5f * voxelSize;
            const float cells = std::ceil(extent[axis] / voxelSize + 1.0f);
            const float clamped = (std::min)((std::max)(cells, 1.0f), static_cast<float>(maxDim));
            dims[axis] = static_cast<uint32_t>(clamped);
        }
        desc.dimX = dims[0];
        desc.dimY = dims[1];
        desc.dimZ = dims[2];
        return desc;
    }
}
