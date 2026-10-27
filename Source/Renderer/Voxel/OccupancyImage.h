#pragma once

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    enum class VoxelAxis : uint8_t
    {
        X,
        Y,
        Z,
    };

    enum class OccupancyImageMode : uint8_t
    {
        // One voxel layer perpendicular to the axis: 255 where occupied, 0 elsewhere.
        Slice,
        // Every layer collapsed along the axis: the number of occupied voxels in each column,
        // scaled so the densest column is 255 and any non-empty column is at least 1.
        Projection,
    };

    // Grayscale view of an OccupancyBitGrid, row-major, row 0 at the top.
    // Image axes per view axis (u = column, v = row):
    //   X: u = z, v = y flipped (y up)
    //   Y: u = x, v = z (top-down)
    //   Z: u = x, v = y flipped (y up)
    struct OccupancyImage
    {
        uint32_t width = 0;
        uint32_t height = 0;
        // Image size before downsampling; width/height divide these by `downsample`, rounded up.
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint32_t downsample = 1;
        std::vector<uint8_t> values; // width * height
    };

    // Number of voxel layers along `axis` (the valid slice range is [0, AxisDim)).
    uint32_t AxisDim(const OccupancyBitGrid& grid, VoxelAxis axis);

    // Builds the view. sliceIndex is clamped to the axis range and ignored for Projection.
    // When either side exceeds maxSide the image is shrunk by the smallest integer factor that
    // fits; a shrunk pixel keeps the maximum of its block, so thin walls do not vanish.
    // maxSide == 0 disables shrinking. An empty grid gives an empty image.
    OccupancyImage BuildOccupancyImage(const OccupancyBitGrid& grid,
                                       VoxelAxis axis,
                                       OccupancyImageMode mode,
                                       uint32_t sliceIndex,
                                       uint32_t maxSide);
}

namespace SasamiEngine { using namespace Render; }
