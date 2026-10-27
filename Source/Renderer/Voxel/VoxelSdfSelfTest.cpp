#include "Renderer/Voxel/VoxelSdfSelfTest.h"

#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SdfSphereTracer.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"

#include <cmath>
#include <cstdint>
#include <sstream>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        // PCG-style hash so the grids are the same on every run.
        uint32_t Hash(uint32_t value)
        {
            uint32_t state = value * 747796405u + 2891336453u;
            const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
            return (word >> 22u) ^ word;
        }

        // Signed distance of one voxel by visiting every other voxel (same convention as SignedDistanceGrid).
        float BruteForce(const OccupancyBitGrid& grid, uint32_t x, uint32_t y, uint32_t z, float noFeature)
        {
            const bool occupied = grid.Test(x, y, z);
            int64_t best = -1;
            for (uint32_t cz = 0; cz < grid.DimZ(); ++cz) {
                for (uint32_t cy = 0; cy < grid.DimY(); ++cy) {
                    for (uint32_t cx = 0; cx < grid.DimX(); ++cx) {
                        if (grid.Test(cx, cy, cz) == occupied) {
                            continue;
                        }
                        const int64_t dx = static_cast<int64_t>(cx) - x;
                        const int64_t dy = static_cast<int64_t>(cy) - y;
                        const int64_t dz = static_cast<int64_t>(cz) - z;
                        const int64_t squared = dx * dx + dy * dy + dz * dz;
                        if (best < 0 || squared < best) {
                            best = squared;
                        }
                    }
                }
            }
            if (best < 0) {
                return occupied ? -noFeature : noFeature;
            }
            const float distance = std::sqrt(static_cast<float>(best)) - 0.5f;
            return occupied ? -distance : distance;
        }

        // Number of voxels whose field value differs from the brute force.
        uint64_t CountBruteForceMismatches(const OccupancyBitGrid& grid, const SignedDistanceGrid& field)
        {
            uint64_t mismatches = 0;
            for (uint32_t z = 0; z < grid.DimZ(); ++z) {
                for (uint32_t y = 0; y < grid.DimY(); ++y) {
                    for (uint32_t x = 0; x < grid.DimX(); ++x) {
                        if (field.Get(x, y, z) != BruteForce(grid, x, y, z, field.NoFeatureDistance())) {
                            ++mismatches;
                        }
                    }
                }
            }
            return mismatches;
        }
    }

    std::string RunVoxelSdfSelfTest()
    {
        uint32_t checks = 0;
        uint64_t voxelsCompared = 0;
        std::string firstError;
        auto check = [&](bool condition, const std::string& description)
        {
            ++checks;
            if (!condition && firstError.empty()) {
                firstError = description;
            }
        };

        // (a) Random occupancy. Odd, unequal dims so a swapped axis or stride shows up.
        const uint32_t dims[][3] = { { 13, 7, 9 }, { 1, 17, 5 }, { 11, 1, 1 }, { 6, 10, 3 } };
        const uint32_t thresholdsPercent[] = { 3, 30, 70, 97 };
        uint32_t seed = 1;
        for (const auto& d : dims) {
            for (uint32_t threshold : thresholdsPercent) {
                OccupancyBitGrid grid(d[0], d[1], d[2]);
                for (uint32_t z = 0; z < d[2]; ++z)
                    for (uint32_t y = 0; y < d[1]; ++y)
                        for (uint32_t x = 0; x < d[0]; ++x)
                            if (Hash(seed++) % 100u < threshold)
                                grid.Set(x, y, z);

                SignedDistanceGrid field;
                BuildSignedDistanceField(grid, field);
                std::ostringstream name;
                name << "random " << d[0] << "x" << d[1] << "x" << d[2] << " at " << threshold << "%";
                check(field.CountSignMismatches(grid) == 0, name.str() + ": sign follows occupancy");
                const uint64_t mismatches = CountBruteForceMismatches(grid, field);
                std::ostringstream detail;
                detail << name.str() << ": " << mismatches << " voxels differ from brute force";
                check(mismatches == 0, detail.str());
                voxelsCompared += grid.VoxelCount();
            }
        }

        // (b) One occupied voxel: the field is the plain Euclidean distance to it, minus a half.
        {
            OccupancyBitGrid grid(9, 9, 9);
            grid.Set(4, 4, 4);
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            check(field.Get(4, 4, 4) == -0.5f, "single: the occupied voxel reads -0.5");
            check(field.Get(5, 4, 4) == 0.5f, "single: a face neighbour reads +0.5");
            check(field.Get(0, 0, 0) == std::sqrt(48.0f) - 0.5f, "single: the corner reads sqrt(48) - 0.5");
        }

        // (c) No feature on one side.
        {
            OccupancyBitGrid empty(5, 4, 3);
            SignedDistanceGrid field;
            BuildSignedDistanceField(empty, field);
            check(field.Get(2, 2, 1) == field.NoFeatureDistance(), "all empty: +NoFeatureDistance");

            OccupancyBitGrid full(5, 4, 3);
            for (uint32_t z = 0; z < 3; ++z)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 5; ++x)
                        full.Set(x, y, z);
            BuildSignedDistanceField(full, field);
            check(field.Get(2, 2, 1) == -field.NoFeatureDistance(), "all occupied: -NoFeatureDistance");
            check(field.CountSignMismatches(full) == 0, "all occupied: sign follows occupancy");
        }

        // (d) Empty input and dimension mismatch.
        {
            SignedDistanceGrid field;
            BuildSignedDistanceField(OccupancyBitGrid(), field);
            check(field.VoxelCount() == 0 && field.Values().empty(), "empty input: empty field");
            check(field.CountSignMismatches(OccupancyBitGrid(2, 2, 2)) == UINT64_MAX, "dims mismatch: UINT64_MAX");
        }

        // (e) Sphere tracing (SdfSphereTracer) against a voxelized half-space: voxels with y < 8 are occupied,
        //     so the zero level is the plane y = 8 voxels. Grid placed at origin (-4, 1, 2), 0.5 m voxels.
        {
            OccupancyBitGrid grid(16, 16, 16);
            for (uint32_t z = 0; z < 16; ++z)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 16; ++x)
                        grid.Set(x, y, z);
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            check(SampleSdfTrilinear(field, 3.5f, 8.0f, 7.5f) == 0.0f, "trace: trilinear field is 0 on the face plane");
            check(SampleSdfTrilinear(field, 3.5f, 10.5f, 7.5f) == 2.5f, "trace: trilinear field is 2.5 at 2.5 voxels above");

            VoxelGridDesc desc;
            desc.origin[0] = -4.0f;
            desc.origin[1] = 1.0f;
            desc.origin[2] = 2.0f;
            desc.voxelSize = 0.5f;
            desc.dimX = desc.dimY = desc.dimZ = 16;
            const float planeY = desc.origin[1] + 8.0f * desc.voxelSize;
            const SdfTraceSettings settings;

            // Straight down from 3 m above the plane, outside the grid box (the slab test must clip in).
            const float above[3] = { 0.1f, planeY + 3.0f + 8.0f * desc.voxelSize, 5.3f };
            const float down[3] = { 0.0f, -1.0f, 0.0f };
            const SdfRayHit vertical = TraceSdfRay(field, desc, above, down, settings);
            const float verticalExpected = above[1] - planeY;
            check(vertical.hit && std::fabs(vertical.t - verticalExpected) <= settings.hitEpsilon * desc.voxelSize,
                  "trace: vertical ray hits the plane at the analytic t");
            check(vertical.hit && vertical.normal[1] > 0.99f, "trace: vertical hit normal points up");

            // Oblique, unnormalized direction (the tracer normalizes it).
            const float start[3] = { -3.0f, planeY + 2.0f, 3.0f };
            const float oblique[3] = { 2.0f, -2.0f, 1.0f };
            const SdfRayHit slanted = TraceSdfRay(field, desc, start, oblique, settings);
            const float obliqueExpected = 2.0f / (2.0f / 3.0f);
            check(slanted.hit && std::fabs(slanted.t - obliqueExpected) <= settings.hitEpsilon * desc.voxelSize * 1.5f + 1.0e-4f,
                  "trace: oblique ray hits the plane at the analytic t");

            // Pointing up and away from the box: both miss.
            const float up[3] = { 0.0f, 1.0f, 0.0f };
            check(!TraceSdfRay(field, desc, start, up, settings).hit, "trace: ray leaving the surface misses");
            const float outside[3] = { 100.0f, 100.0f, 100.0f };
            check(!TraceSdfRay(field, desc, outside, down, settings).hit, "trace: ray outside the grid box misses");
        }

        std::ostringstream out;
        if (firstError.empty()) {
            out << "OK checks=" << checks << " voxelsCompared=" << voxelsCompared;
        } else {
            out << "ERR " << firstError << " (checks=" << checks << ")";
        }
        return out.str();
    }
}
