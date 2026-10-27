#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class SignedDistanceGrid;

    // Sphere tracing (Hart 1996) through a SignedDistanceGrid placed by a VoxelGridDesc.
    // The field is sampled trilinearly between voxel centres, with coordinates clamped to the
    // grid, so a ray only marches inside the grid AABB (slab test first). Distances are in voxel
    // units; world t = voxel t * voxelSize. CPU reference for a later GPU pass (5e-3b).
    struct SdfTraceSettings
    {
        uint32_t maxSteps = 256;
        float hitEpsilon = 0.01f;   // voxels
        float stepScale = 0.9f;     // trilinear blending is not exactly 1-Lipschitz
        float minStep = 0.01f;      // voxels, keeps grazing rays moving
    };

    struct SdfRayHit
    {
        bool hit = false;
        float t = 0.0f;             // world units along the normalized direction
        uint32_t steps = 0;
        float normal[3] = { 0.0f, 0.0f, 0.0f };
    };

    // Field value in voxel units at a point in voxel space (voxel (x,y,z) has its centre at x+0.5 ...).
    float SampleSdfTrilinear(const SignedDistanceGrid& field, float vx, float vy, float vz);

    // `direction` need not be normalized. Returns hit=false for an empty field or a missed ray.
    SdfRayHit TraceSdfRay(const SignedDistanceGrid& field, const VoxelGridDesc& desc,
                          const float origin[3], const float direction[3], const SdfTraceSettings& settings);

    struct SdfTraceCamera
    {
        float position[3] = { 0.0f, 0.0f, 0.0f };
        float right[3] = { 1.0f, 0.0f, 0.0f };
        float up[3] = { 0.0f, 1.0f, 0.0f };
        float forward[3] = { 0.0f, 0.0f, 1.0f };
        float tanHalfFovY = 0.577350269f;
        float aspectRatio = 1.0f;
    };

    struct SdfTraceImageStats
    {
        uint64_t hitCount = 0;
        uint64_t totalSteps = 0;
        uint32_t maxSteps = 0;
    };

    // Lambert-shaded view of the field, BGRA8 top row first (PngWriter order). Misses are dark blue.
    // outDepth, when given, receives the world-space ray distance per pixel (+inf on a miss).
    void RenderSdfTraceImage(const SignedDistanceGrid& field, const VoxelGridDesc& desc, const SdfTraceCamera& camera,
                             uint32_t width, uint32_t height, const SdfTraceSettings& settings,
                             std::vector<uint8_t>& outBgra, SdfTraceImageStats& outStats,
                             std::vector<float>* outDepth = nullptr);
}

namespace SasamiEngine { using namespace Render; }
