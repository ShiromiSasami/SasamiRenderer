#pragma once

#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    class OccupancyBitGrid;
    class SignedDistanceGrid;

    // Clamped SDF of one chunk from an occupancy grid that covers the chunk plus an apron
    // (6b stage 2). A voxel whose nearest feature centre lies within distance A of it has that
    // feature inside a box of half-width A around it, so if the apron is A voxels wide (or reaches
    // the grid edge) the local transform is exact wherever |d| <= A - 0.5. Farther values are only
    // known to exceed that, so every value is clamped to [-(A - 0.5), +(A - 0.5)]; the result then
    // equals the clamped whole-grid SDF bit for bit.
    float ChunkSdfLimit(uint32_t apronVoxels);

    // expanded: occupancy of the chunk plus apron. coreOffset/coreSize: the chunk inside it.
    // outCore receives the chunk's occupancy, outSdf its clamped SDF (both coreSize).
    void BuildChunkSignedDistance(const OccupancyBitGrid& expanded, const uint32_t coreOffset[3],
                                  const uint32_t coreSize[3], uint32_t apronVoxels,
                                  OccupancyBitGrid& outCore, SignedDistanceGrid& outSdf);

    // Chunk with no occupied voxel in its expanded box: every value is +limit.
    void FillEmptyChunkSignedDistance(const uint32_t coreSize[3], uint32_t apronVoxels, SignedDistanceGrid& outSdf);

    // Copies voxels offset .. offset + size - 1 of src into dst (Reset to size).
    void CopyOccupancyRegion(const OccupancyBitGrid& src, const uint32_t offset[3], const uint32_t size[3],
                             OccupancyBitGrid& dst);
}

namespace SasamiEngine { using namespace Render; }
