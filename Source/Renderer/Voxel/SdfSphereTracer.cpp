#include "Renderer/Voxel/SdfSphereTracer.h"

#include "Foundation/Jobs/ParallelFor.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        float Clamp(float value, float low, float high)
        {
            return value < low ? low : (value > high ? high : value);
        }

        // Slab test against [0, dim] in voxel space. False when the ray misses or the box is behind it.
        bool IntersectGridBox(const SignedDistanceGrid& field, const float origin[3], const float direction[3],
                              float& outNear, float& outFar)
        {
            const float dims[3] = { static_cast<float>(field.DimX()), static_cast<float>(field.DimY()),
                                    static_cast<float>(field.DimZ()) };
            float tNear = 0.0f;
            float tFar = 3.0e38f;
            for (int axis = 0; axis < 3; ++axis) {
                if (std::fabs(direction[axis]) < 1.0e-12f) {
                    if (origin[axis] < 0.0f || origin[axis] > dims[axis]) {
                        return false;
                    }
                    continue;
                }
                const float inv = 1.0f / direction[axis];
                float t0 = (0.0f - origin[axis]) * inv;
                float t1 = (dims[axis] - origin[axis]) * inv;
                if (t0 > t1) {
                    std::swap(t0, t1);
                }
                tNear = std::max(tNear, t0);
                tFar = std::min(tFar, t1);
                if (tNear > tFar) {
                    return false;
                }
            }
            outNear = tNear;
            outFar = tFar;
            return true;
        }

        void Normalize3(float v[3])
        {
            const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length > 0.0f) {
                v[0] /= length;
                v[1] /= length;
                v[2] /= length;
            }
        }
    }

    float SampleSdfTrilinear(const SignedDistanceGrid& field, float vx, float vy, float vz)
    {
        if (field.VoxelCount() == 0) {
            return 0.0f;
        }
        // Voxel centres sit at integer + 0.5; shift so they land on integers, then clamp.
        const float u[3] = { Clamp(vx - 0.5f, 0.0f, static_cast<float>(field.DimX() - 1)),
                             Clamp(vy - 0.5f, 0.0f, static_cast<float>(field.DimY() - 1)),
                             Clamp(vz - 0.5f, 0.0f, static_cast<float>(field.DimZ() - 1)) };
        const uint32_t limit[3] = { field.DimX() - 1, field.DimY() - 1, field.DimZ() - 1 };
        uint32_t i0[3];
        uint32_t i1[3];
        float f[3];
        for (int axis = 0; axis < 3; ++axis) {
            const float base = std::floor(u[axis]);
            i0[axis] = static_cast<uint32_t>(base);
            i1[axis] = std::min(i0[axis] + 1, limit[axis]);
            f[axis] = u[axis] - base;
        }
        const float c000 = field.Get(i0[0], i0[1], i0[2]);
        const float c100 = field.Get(i1[0], i0[1], i0[2]);
        const float c010 = field.Get(i0[0], i1[1], i0[2]);
        const float c110 = field.Get(i1[0], i1[1], i0[2]);
        const float c001 = field.Get(i0[0], i0[1], i1[2]);
        const float c101 = field.Get(i1[0], i0[1], i1[2]);
        const float c011 = field.Get(i0[0], i1[1], i1[2]);
        const float c111 = field.Get(i1[0], i1[1], i1[2]);
        const float x00 = c000 + (c100 - c000) * f[0];
        const float x10 = c010 + (c110 - c010) * f[0];
        const float x01 = c001 + (c101 - c001) * f[0];
        const float x11 = c011 + (c111 - c011) * f[0];
        const float y0 = x00 + (x10 - x00) * f[1];
        const float y1 = x01 + (x11 - x01) * f[1];
        return y0 + (y1 - y0) * f[2];
    }

    SdfRayHit TraceSdfRay(const SignedDistanceGrid& field, const VoxelGridDesc& desc,
                          const float origin[3], const float direction[3], const SdfTraceSettings& settings)
    {
        SdfRayHit result;
        if (field.VoxelCount() == 0 || desc.voxelSize <= 0.0f) {
            return result;
        }
        float dir[3] = { direction[0], direction[1], direction[2] };
        Normalize3(dir);
        const float inv = 1.0f / desc.voxelSize;
        // Uniform scale, so the normalized direction stays normalized in voxel space.
        const float o[3] = { (origin[0] - desc.origin[0]) * inv, (origin[1] - desc.origin[1]) * inv,
                             (origin[2] - desc.origin[2]) * inv };
        float tNear = 0.0f;
        float tFar = 0.0f;
        if (!IntersectGridBox(field, o, dir, tNear, tFar)) {
            return result;
        }

        float t = tNear;
        for (uint32_t step = 0; step < settings.maxSteps && t <= tFar; ++step) {
            const float p[3] = { o[0] + dir[0] * t, o[1] + dir[1] * t, o[2] + dir[2] * t };
            const float d = SampleSdfTrilinear(field, p[0], p[1], p[2]);
            result.steps = step + 1;
            if (d < settings.hitEpsilon) {
                result.hit = true;
                result.t = t * desc.voxelSize;
                // Central differences, one voxel apart.
                result.normal[0] = SampleSdfTrilinear(field, p[0] + 1.0f, p[1], p[2]) -
                                   SampleSdfTrilinear(field, p[0] - 1.0f, p[1], p[2]);
                result.normal[1] = SampleSdfTrilinear(field, p[0], p[1] + 1.0f, p[2]) -
                                   SampleSdfTrilinear(field, p[0], p[1] - 1.0f, p[2]);
                result.normal[2] = SampleSdfTrilinear(field, p[0], p[1], p[2] + 1.0f) -
                                   SampleSdfTrilinear(field, p[0], p[1], p[2] - 1.0f);
                Normalize3(result.normal);
                return result;
            }
            t += std::max(d * settings.stepScale, settings.minStep);
        }
        return result;
    }

    void RenderSdfTraceImage(const SignedDistanceGrid& field, const VoxelGridDesc& desc, const SdfTraceCamera& camera,
                             uint32_t width, uint32_t height, const SdfTraceSettings& settings,
                             std::vector<uint8_t>& outBgra, SdfTraceImageStats& outStats,
                             std::vector<float>* outDepth)
    {
        outBgra.assign(static_cast<size_t>(width) * height * 4, 0);
        if (outDepth) {
            outDepth->assign(static_cast<size_t>(width) * height, std::numeric_limits<float>::infinity());
        }
        outStats = SdfTraceImageStats{};
        if (width == 0 || height == 0) {
            return;
        }

        float light[3] = { 0.4f, 0.8f, 0.3f };
        Normalize3(light);
        std::vector<SdfTraceImageStats> rowStats(height);
        Foundation::ParallelFor(height, [&](uint32_t row) {
            SdfTraceImageStats& stats = rowStats[row];
            const float ndcY = 1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / static_cast<float>(height);
            for (uint32_t column = 0; column < width; ++column) {
                const float ndcX = 2.0f * (static_cast<float>(column) + 0.5f) / static_cast<float>(width) - 1.0f;
                const float sx = ndcX * camera.tanHalfFovY * camera.aspectRatio;
                const float sy = ndcY * camera.tanHalfFovY;
                const float dir[3] = { camera.forward[0] + camera.right[0] * sx + camera.up[0] * sy,
                                       camera.forward[1] + camera.right[1] * sx + camera.up[1] * sy,
                                       camera.forward[2] + camera.right[2] * sx + camera.up[2] * sy };
                const SdfRayHit hit = TraceSdfRay(field, desc, camera.position, dir, settings);
                if (outDepth && hit.hit) {
                    (*outDepth)[static_cast<size_t>(row) * width + column] = hit.t;
                }
                stats.totalSteps += hit.steps;
                stats.maxSteps = std::max(stats.maxSteps, hit.steps);
                uint8_t* pixel = outBgra.data() + (static_cast<size_t>(row) * width + column) * 4;
                if (!hit.hit) {
                    pixel[0] = 60;
                    pixel[1] = 30;
                    pixel[2] = 20;
                    pixel[3] = 255;
                    continue;
                }
                ++stats.hitCount;
                const float lambert = std::max(0.0f, hit.normal[0] * light[0] + hit.normal[1] * light[1] +
                                                         hit.normal[2] * light[2]);
                // Normal-tinted albedo so faces with different orientations stay apart.
                const float shade = 0.2f + 0.8f * lambert;
                const float albedo[3] = { 0.55f + 0.35f * std::fabs(hit.normal[0]),
                                          0.55f + 0.35f * std::fabs(hit.normal[1]),
                                          0.55f + 0.35f * std::fabs(hit.normal[2]) };
                pixel[0] = static_cast<uint8_t>(Clamp(albedo[2] * shade, 0.0f, 1.0f) * 255.0f + 0.5f);
                pixel[1] = static_cast<uint8_t>(Clamp(albedo[1] * shade, 0.0f, 1.0f) * 255.0f + 0.5f);
                pixel[2] = static_cast<uint8_t>(Clamp(albedo[0] * shade, 0.0f, 1.0f) * 255.0f + 0.5f);
                pixel[3] = 255;
            }
        });
        for (const SdfTraceImageStats& stats : rowStats) {
            outStats.hitCount += stats.hitCount;
            outStats.totalSteps += stats.totalSteps;
            outStats.maxSteps = std::max(outStats.maxSteps, stats.maxSteps);
        }
    }
}
