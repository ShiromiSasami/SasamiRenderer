#include "Renderer/Voxel/VoxelDistanceTransform.h"

#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"

#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        // The reference implementation's INF. For dims <= 1024 per axis, every finite f + q^2
        // stays below 4 * 1024^2 < 2^24, so finite results are exact integers in float.
        constexpr float INFINITE_SQUARED = 1e20f;

        // Scratch for one 1-D pass; sized once per axis so the inner loops do not allocate.
        struct LineScratch
        {
            std::vector<float> f;
            std::vector<float> d;
            std::vector<int> v;
            std::vector<float> z;

            void Resize(uint32_t n)
            {
                f.resize(n);
                d.resize(n);
                v.resize(n);
                z.resize(static_cast<size_t>(n) + 1);
            }
        };

        // Abscissa where the parabolas rooted at q and p meet.
        float Intersection(const float* f, int q, int p)
        {
            return ((f[q] + static_cast<float>(q) * q) - (f[p] + static_cast<float>(p) * p))
                 / static_cast<float>(2 * q - 2 * p);
        }

        // Lower envelope of the parabolas rooted at (q, f[q]) (Felzenszwalb & Huttenlocher 2012, fig. 2).
        void DistanceTransform1D(LineScratch& s, uint32_t n)
        {
            const float* f = s.f.data();
            int k = 0;
            s.v[0] = 0;
            s.z[0] = -INFINITE_SQUARED;
            s.z[1] = INFINITE_SQUARED;
            // Every intersection is > -INFINITE_SQUARED / 2 (|numerator| <= ~INFINITE_SQUARED and
            // the denominator is >= 2), so the loop never steps below k = 0.
            for (int q = 1; q < static_cast<int>(n); ++q) {
                float intersection = Intersection(f, q, s.v[k]);
                while (intersection <= s.z[k]) {
                    --k;
                    intersection = Intersection(f, q, s.v[k]);
                }
                ++k;
                s.v[k] = q;
                s.z[k] = intersection;
                s.z[k + 1] = INFINITE_SQUARED;
            }
            k = 0;
            for (int q = 0; q < static_cast<int>(n); ++q) {
                while (s.z[k + 1] < static_cast<float>(q)) {
                    ++k;
                }
                const float delta = static_cast<float>(q - s.v[k]);
                s.d[q] = delta * delta + f[s.v[k]];
            }
        }

        // Runs the 1-D transform over every line along one axis. stride is the step between
        // samples of a line, and the lines are enumerated by (outerCount x innerCount) with
        // the given strides.
        void TransformAxis(std::vector<float>& values, uint32_t n, size_t stride,
                           uint32_t innerCount, size_t innerStride,
                           uint32_t outerCount, size_t outerStride)
        {
            LineScratch scratch;
            scratch.Resize(n);
            for (uint32_t outer = 0; outer < outerCount; ++outer) {
                for (uint32_t inner = 0; inner < innerCount; ++inner) {
                    const size_t base = outer * outerStride + inner * innerStride;
                    for (uint32_t i = 0; i < n; ++i) {
                        scratch.f[i] = values[base + i * stride];
                    }
                    DistanceTransform1D(scratch, n);
                    for (uint32_t i = 0; i < n; ++i) {
                        values[base + i * stride] = scratch.d[i];
                    }
                }
            }
        }
    }

    float SquaredDistanceInfinity()
    {
        return INFINITE_SQUARED;
    }

    void SquaredDistanceTransform3D(std::vector<float>& values, uint32_t dimX, uint32_t dimY, uint32_t dimZ)
    {
        if (dimX == 0 || dimY == 0 || dimZ == 0) {
            return;
        }
        const size_t sliceStride = static_cast<size_t>(dimX) * dimY;
        TransformAxis(values, dimX, 1, dimY, dimX, dimZ, sliceStride);
        TransformAxis(values, dimY, dimX, dimX, 1, dimZ, sliceStride);
        TransformAxis(values, dimZ, sliceStride, dimX, 1, dimY, dimX);
    }

    void BuildSignedDistanceField(const OccupancyBitGrid& occupancy, SignedDistanceGrid& out)
    {
        const uint32_t dimX = occupancy.DimX();
        const uint32_t dimY = occupancy.DimY();
        const uint32_t dimZ = occupancy.DimZ();
        out.Reset(dimX, dimY, dimZ);
        if (out.VoxelCount() == 0) {
            return;
        }

        const size_t count = static_cast<size_t>(out.VoxelCount());
        std::vector<float> toOccupied(count);
        std::vector<float> toEmpty(count);
        size_t i = 0;
        for (uint32_t z = 0; z < dimZ; ++z) {
            for (uint32_t y = 0; y < dimY; ++y) {
                for (uint32_t x = 0; x < dimX; ++x, ++i) {
                    const bool occupied = occupancy.Test(x, y, z);
                    toOccupied[i] = occupied ? 0.0f : INFINITE_SQUARED;
                    toEmpty[i] = occupied ? INFINITE_SQUARED : 0.0f;
                }
            }
        }
        SquaredDistanceTransform3D(toOccupied, dimX, dimY, dimZ);
        SquaredDistanceTransform3D(toEmpty, dimX, dimY, dimZ);

        const float noFeature = out.NoFeatureDistance();
        const float noFeatureThreshold = INFINITE_SQUARED * 0.5f;
        std::vector<float>& values = out.MutableValues();
        for (size_t j = 0; j < count; ++j) {
            if (toOccupied[j] == 0.0f) {
                values[j] = (toEmpty[j] >= noFeatureThreshold) ? -noFeature : -(std::sqrt(toEmpty[j]) - 0.5f);
            } else {
                values[j] = (toOccupied[j] >= noFeatureThreshold) ? noFeature : std::sqrt(toOccupied[j]) - 0.5f;
            }
        }
    }
}
