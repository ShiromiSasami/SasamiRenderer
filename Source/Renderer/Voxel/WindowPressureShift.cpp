#include "Renderer/Voxel/WindowPressureShift.h"

#include <algorithm>
#include <cstring>

namespace SasamiEngine::Render::Voxel
{
    uint64_t ShiftWindowPressure(const std::vector<float>& oldP, const VoxelRegion& oldRegion,
                                 const VoxelRegion& newRegion, std::vector<float>& outP)
    {
        outP.assign(static_cast<size_t>(newRegion.VoxelCount()), 0.0f);
        if (oldP.size() != static_cast<size_t>(oldRegion.VoxelCount()) || !VoxelRegionsOverlap(oldRegion, newRegion)) {
            return 0;
        }
        uint32_t lo[3];
        uint32_t hi[3];   // exclusive, anchor-grid voxels
        for (int a = 0; a < 3; ++a) {
            lo[a] = (std::max)(oldRegion.min[a], newRegion.min[a]);
            hi[a] = (std::min)(oldRegion.min[a] + oldRegion.size[a], newRegion.min[a] + newRegion.size[a]);
        }
        const uint32_t rowLength = hi[0] - lo[0];
        for (uint32_t z = lo[2]; z < hi[2]; ++z) {
            for (uint32_t y = lo[1]; y < hi[1]; ++y) {
                const size_t src = (static_cast<size_t>(z - oldRegion.min[2]) * oldRegion.size[1] + (y - oldRegion.min[1]))
                                       * oldRegion.size[0] + (lo[0] - oldRegion.min[0]);
                const size_t dst = (static_cast<size_t>(z - newRegion.min[2]) * newRegion.size[1] + (y - newRegion.min[1]))
                                       * newRegion.size[0] + (lo[0] - newRegion.min[0]);
                std::memcpy(&outP[dst], &oldP[src], rowLength * sizeof(float));
            }
        }
        return static_cast<uint64_t>(rowLength) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    }
}
