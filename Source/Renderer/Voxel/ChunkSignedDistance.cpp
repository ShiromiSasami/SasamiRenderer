#include "Renderer/Voxel/ChunkSignedDistance.h"

#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"

#include <algorithm>

namespace SasamiEngine::Render::Voxel
{
    float ChunkSdfLimit(uint32_t apronVoxels)
    {
        return (std::max)(static_cast<float>(apronVoxels) - 0.5f, 0.5f);
    }

    void CopyOccupancyRegion(const OccupancyBitGrid& src, const uint32_t offset[3], const uint32_t size[3],
                             OccupancyBitGrid& dst)
    {
        dst.Reset(size[0], size[1], size[2]);
        for (uint32_t z = 0; z < size[2]; ++z)
        {
            for (uint32_t y = 0; y < size[1]; ++y)
            {
                for (uint32_t x = 0; x < size[0]; ++x)
                {
                    if (src.Test(offset[0] + x, offset[1] + y, offset[2] + z))
                    {
                        dst.Set(x, y, z);
                    }
                }
            }
        }
    }

    void BuildChunkSignedDistance(const OccupancyBitGrid& expanded, const uint32_t coreOffset[3],
                                  const uint32_t coreSize[3], uint32_t apronVoxels,
                                  OccupancyBitGrid& outCore, SignedDistanceGrid& outSdf)
    {
        CopyOccupancyRegion(expanded, coreOffset, coreSize, outCore);

        SignedDistanceGrid full;
        BuildSignedDistanceField(expanded, full);
        const float limit = ChunkSdfLimit(apronVoxels);
        outSdf.Reset(coreSize[0], coreSize[1], coreSize[2]);
        for (uint32_t z = 0; z < coreSize[2]; ++z)
        {
            for (uint32_t y = 0; y < coreSize[1]; ++y)
            {
                for (uint32_t x = 0; x < coreSize[0]; ++x)
                {
                    const float d = full.Get(coreOffset[0] + x, coreOffset[1] + y, coreOffset[2] + z);
                    outSdf.Set(x, y, z, std::clamp(d, -limit, limit));
                }
            }
        }
    }

    void FillEmptyChunkSignedDistance(const uint32_t coreSize[3], uint32_t apronVoxels, SignedDistanceGrid& outSdf)
    {
        outSdf.Reset(coreSize[0], coreSize[1], coreSize[2]);
        std::fill(outSdf.MutableValues().begin(), outSdf.MutableValues().end(), ChunkSdfLimit(apronVoxels));
    }
}
