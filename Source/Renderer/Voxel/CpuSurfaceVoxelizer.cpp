#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/TriangleBoxOverlap.h"
#include "Renderer/Voxel/VoxelRegion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    VoxelizeStats VoxelizeSurface(const VoxelGridDesc& desc,
                                  const VoxelizerMeshInput& mesh,
                                  OccupancyBitGrid& grid)
    {
        const bool invalid = desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0
            || !(desc.voxelSize > 0.0f)
            || mesh.positions == nullptr
            || grid.DimX() != desc.dimX || grid.DimY() != desc.dimY || grid.DimZ() != desc.dimZ;
        if (invalid)
        {
            VoxelizeStats stats;
            stats.occupiedVoxels = grid.CountOccupied();
            return stats;
        }

        return VoxelizeSurfaceRegion(desc, FullVoxelRegion(desc), mesh, grid);
    }

    VoxelizeStats VoxelizeSurfaceRegion(const VoxelGridDesc& desc,
                                        const VoxelRegion& region,
                                        const VoxelizerMeshInput& mesh,
                                        OccupancyBitGrid& regionGrid)
    {
        VoxelizeStats stats;

        const bool invalid = desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0
            || !(desc.voxelSize > 0.0f)
            || mesh.positions == nullptr
            || region.IsEmpty()
            || static_cast<uint64_t>(region.min[0]) + region.size[0] > desc.dimX
            || static_cast<uint64_t>(region.min[1]) + region.size[1] > desc.dimY
            || static_cast<uint64_t>(region.min[2]) + region.size[2] > desc.dimZ
            || regionGrid.DimX() != region.size[0] || regionGrid.DimY() != region.size[1]
            || regionGrid.DimZ() != region.size[2];
        if (invalid)
        {
            return stats;
        }

        const uint32_t triangleCount = (mesh.indices != nullptr ? mesh.indexCount : mesh.vertexCount) / 3;
        const uint32_t dims[3] = { desc.dimX, desc.dimY, desc.dimZ };
        const int64_t regionMin[3] = { region.min[0], region.min[1], region.min[2] };
        const float voxelSize = desc.voxelSize;
        const float halfSize[3] = { 0.5f * voxelSize, 0.5f * voxelSize, 0.5f * voxelSize };

        for (uint32_t tri = 0; tri < triangleCount; ++tri)
        {
            uint32_t vertexIndex[3];
            bool outOfRange = false;
            for (uint32_t k = 0; k < 3; ++k)
            {
                vertexIndex[k] = mesh.indices != nullptr ? mesh.indices[tri * 3 + k] : tri * 3 + k;
                if (vertexIndex[k] >= mesh.vertexCount)
                {
                    outOfRange = true;
                }
            }
            if (outOfRange)
            {
                ++stats.trianglesSkipped;
                continue;
            }

            const float* v[3];
            for (uint32_t k = 0; k < 3; ++k)
            {
                v[k] = mesh.positions + static_cast<size_t>(vertexIndex[k]) * mesh.positionStrideInFloats;
            }

            int64_t lo[3];
            int64_t hi[3];
            bool outside = false;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                const float vMin = std::min(v[0][axis], std::min(v[1][axis], v[2][axis]));
                const float vMax = std::max(v[0][axis], std::max(v[1][axis], v[2][axis]));
                const double tMin = (static_cast<double>(vMin) - desc.origin[axis]) / voxelSize;
                const double tMax = (static_cast<double>(vMax) - desc.origin[axis]) / voxelSize;
                const int64_t maxIndex = static_cast<int64_t>(dims[axis]) - 1;
                lo[axis] = std::max<int64_t>(static_cast<int64_t>(std::ceil(tMin)) - 1, 0);
                hi[axis] = std::min<int64_t>(static_cast<int64_t>(std::floor(tMax)), maxIndex);
                lo[axis] = std::max<int64_t>(lo[axis], regionMin[axis]);
                hi[axis] = std::min<int64_t>(hi[axis], regionMin[axis] + static_cast<int64_t>(region.size[axis]) - 1);
                if (lo[axis] > hi[axis])
                {
                    outside = true;
                }
            }
            if (outside)
            {
                ++stats.trianglesSkipped;
                continue;
            }

            ++stats.trianglesTested;
            for (int64_t z = lo[2]; z <= hi[2]; ++z)
            {
                for (int64_t y = lo[1]; y <= hi[1]; ++y)
                {
                    for (int64_t x = lo[0]; x <= hi[0]; ++x)
                    {
                        const float center[3] = {
                            desc.origin[0] + (static_cast<float>(x) + 0.5f) * voxelSize,
                            desc.origin[1] + (static_cast<float>(y) + 0.5f) * voxelSize,
                            desc.origin[2] + (static_cast<float>(z) + 0.5f) * voxelSize
                        };
                        ++stats.boxTests;
                        if (TriangleOverlapsBox(center, halfSize, v[0], v[1], v[2]))
                        {
                            regionGrid.Set(static_cast<uint32_t>(x - regionMin[0]),
                                           static_cast<uint32_t>(y - regionMin[1]),
                                           static_cast<uint32_t>(z - regionMin[2]));
                        }
                    }
                }
            }
        }

        stats.occupiedVoxels = regionGrid.CountOccupied();
        return stats;
    }
}
