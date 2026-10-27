#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class OccupancyBitGrid;
    class SignedDistanceGrid;

    // Exact squared Euclidean distance transform of a sampled function (Felzenszwalb &
    // Huttenlocher, "Distance Transforms of Sampled Functions", Theory of Computing 8, 2012):
    // the 1-D lower envelope of parabolas, applied along x, then y, then z.
    //
    // `values` holds dimX * dimY * dimZ samples in OccupancyBitGrid order. On entry a sample is
    // 0 at a feature and SquaredDistanceInfinity() elsewhere; on return it is the squared
    // distance, in voxels, to the nearest feature centre (still >= SquaredDistanceInfinity() / 2
    // when the grid has no feature at all).
    void SquaredDistanceTransform3D(std::vector<float>& values, uint32_t dimX, uint32_t dimY, uint32_t dimZ);
    float SquaredDistanceInfinity();

    // Signed distance of an occupancy grid with the convention documented on
    // SignedDistanceGrid (two transforms: to the occupied set and to the empty set).
    void BuildSignedDistanceField(const OccupancyBitGrid& occupancy, SignedDistanceGrid& out);
}

namespace SasamiEngine { using namespace Render; }
