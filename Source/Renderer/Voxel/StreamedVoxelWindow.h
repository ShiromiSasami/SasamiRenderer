#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelRegion.h"

#include <cstdint>
#include <string>

namespace SasamiEngine::Render::Voxel
{
    class VoxelChunkStreamer;

    // A dense box of streamed chunks copied out of a VoxelChunkStreamer (6b stage 3a): the input of
    // a wind solve that only covers the chunks around the camera instead of the whole scene grid.
    struct StreamedVoxelWindow
    {
        VoxelGridDesc desc{};            // the window as its own grid (origin moved to region.min)
        VoxelRegion region{};            // the window in anchor-grid voxels
        uint32_t chunkLo[3] = { 0, 0, 0 };
        uint32_t chunkHi[3] = { 0, 0, 0 };   // inclusive
        OccupancyBitGrid occupancy;
        SignedDistanceGrid sdf;          // clamped to +-ChunkSdfLimit(sdfApronVoxels) voxels, voxel units
    };

    // Copies the chunks within Chebyshev `radius` of centreChunk (clamped to the chunk range) into
    // outWindow. Fails, leaving outWindow unspecified, unless the streamer runs with
    // sdfApronVoxels > 0 and every chunk of the box is Loaded; outMissing then counts the chunks
    // that are not. The copy is exact, so the window equals the same box of the anchor grid
    // (occupancy) and of its clamped SDF bit for bit.
    bool AssembleStreamedVoxelWindow(const VoxelChunkStreamer& streamer, const int32_t centreChunk[3], uint32_t radius,
                                     StreamedVoxelWindow& outWindow, uint32_t& outMissing, std::string& outError);
}

namespace SasamiEngine { using namespace Render; }
