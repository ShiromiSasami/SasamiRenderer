#include "Renderer/Voxel/StreamedVoxelWindow.h"

#include "Renderer/Voxel/VoxelChunkStreamer.h"

#include <algorithm>
#include <cstring>

namespace SasamiEngine::Render::Voxel
{
    bool AssembleStreamedVoxelWindow(const VoxelChunkStreamer& streamer, const int32_t centreChunk[3], uint32_t radius,
                                     StreamedVoxelWindow& outWindow, uint32_t& outMissing, std::string& outError)
    {
        outMissing = 0;
        if (!streamer.IsRunning())
        {
            outError = "streamer is not running";
            return false;
        }
        if (streamer.Settings().sdfApronVoxels == 0)
        {
            outError = "streamer has no SDF (start it with sdf=A)";
            return false;
        }
        const VoxelChunkStreamerStats& stats = streamer.Stats();
        const uint32_t chunkCount[3] = { stats.chunkCountX, stats.chunkCountY, stats.chunkCountZ };
        for (int a = 0; a < 3; ++a)
        {
            if (chunkCount[a] == 0)
            {
                outError = "streamer has no chunks";
                return false;
            }
            const int64_t centre = std::clamp<int64_t>(centreChunk[a], 0, static_cast<int64_t>(chunkCount[a]) - 1);
            outWindow.chunkLo[a] = static_cast<uint32_t>((std::max)(centre - static_cast<int64_t>(radius), int64_t{ 0 }));
            outWindow.chunkHi[a] = static_cast<uint32_t>(
                (std::min)(centre + static_cast<int64_t>(radius), static_cast<int64_t>(chunkCount[a]) - 1));
        }

        const VoxelRegion lo = streamer.ChunkRegion(outWindow.chunkLo[0], outWindow.chunkLo[1], outWindow.chunkLo[2]);
        const VoxelRegion hi = streamer.ChunkRegion(outWindow.chunkHi[0], outWindow.chunkHi[1], outWindow.chunkHi[2]);
        const VoxelGridDesc& anchor = streamer.AnchorDesc();
        for (int a = 0; a < 3; ++a)
        {
            outWindow.region.min[a] = lo.min[a];
            outWindow.region.size[a] = hi.min[a] + hi.size[a] - lo.min[a];
            outWindow.desc.origin[a] = anchor.origin[a] + static_cast<float>(lo.min[a]) * anchor.voxelSize;
        }
        outWindow.desc.voxelSize = anchor.voxelSize;
        outWindow.desc.dimX = outWindow.region.size[0];
        outWindow.desc.dimY = outWindow.region.size[1];
        outWindow.desc.dimZ = outWindow.region.size[2];

        const uint32_t expected = (outWindow.chunkHi[0] - outWindow.chunkLo[0] + 1)
            * (outWindow.chunkHi[1] - outWindow.chunkLo[1] + 1) * (outWindow.chunkHi[2] - outWindow.chunkLo[2] + 1);
        uint32_t found = 0;
        streamer.ForEachChunk([&](const VoxelChunkStreamer::ChunkView& view)
        {
            for (int a = 0; a < 3; ++a)
            {
                if (view.coord[a] < outWindow.chunkLo[a] || view.coord[a] > outWindow.chunkHi[a])
                {
                    return;
                }
            }
            if (view.grid != nullptr && view.sdf != nullptr)
            {
                ++found;
            }
        });
        if (found != expected)
        {
            outMissing = expected - found;
            outError = "window chunks not loaded yet";
            return false;
        }

        outWindow.occupancy.Reset(outWindow.desc.dimX, outWindow.desc.dimY, outWindow.desc.dimZ);
        outWindow.sdf.Reset(outWindow.desc.dimX, outWindow.desc.dimY, outWindow.desc.dimZ);
        std::vector<float>& dst = outWindow.sdf.MutableValues();
        const uint64_t dstRow = outWindow.desc.dimX;
        const uint64_t dstSlice = dstRow * outWindow.desc.dimY;
        bool ok = true;
        streamer.ForEachChunk([&](const VoxelChunkStreamer::ChunkView& view)
        {
            for (int a = 0; a < 3; ++a)
            {
                if (view.coord[a] < outWindow.chunkLo[a] || view.coord[a] > outWindow.chunkHi[a])
                {
                    return;
                }
            }
            const VoxelRegion region = streamer.ChunkRegion(view.coord[0], view.coord[1], view.coord[2]);
            const uint32_t offset[3] = { region.min[0] - outWindow.region.min[0], region.min[1] - outWindow.region.min[1],
                                         region.min[2] - outWindow.region.min[2] };
            ok = outWindow.occupancy.WriteRegion(*view.grid, offset) && ok;
            const SignedDistanceGrid& sdf = *view.sdf;
            if (sdf.DimX() != region.size[0] || sdf.DimY() != region.size[1] || sdf.DimZ() != region.size[2])
            {
                ok = false;
                return;
            }
            const std::vector<float>& src = sdf.Values();
            for (uint32_t z = 0; z < region.size[2]; ++z)
            {
                for (uint32_t y = 0; y < region.size[1]; ++y)
                {
                    const uint64_t srcIndex = (static_cast<uint64_t>(z) * region.size[1] + y) * region.size[0];
                    const uint64_t dstIndex = (offset[2] + z) * dstSlice + (offset[1] + y) * dstRow + offset[0];
                    std::memcpy(&dst[dstIndex], &src[srcIndex], sizeof(float) * region.size[0]);
                }
            }
        });
        if (!ok)
        {
            outError = "chunk size does not match its region";
            return false;
        }
        return true;
    }
}
