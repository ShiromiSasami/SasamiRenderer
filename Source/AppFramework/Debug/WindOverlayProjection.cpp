#include "AppFramework/Debug/WindOverlayProjection.h"

#include "Renderer/Voxel/SdfSphereTracer.h"
#include "Foundation/Math/MathUtil.h"

#include <cmath>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        constexpr float MIN_VIEW_DEPTH = 0.05f;
        constexpr float CAMERA_CLEARANCE = 2.0f;  // voxels

        using Foundation::Math::Dot3;
    }

    bool ProjectToOverlay(const Render::RenderCameraProxy& camera, const ImVec2& origin, const ImVec2& size,
                          const float point[3], ImVec2& out, float* outDepth)
    {
        const float rel[3] = { point[0] - camera.cameraPosition[0], point[1] - camera.cameraPosition[1],
                               point[2] - camera.cameraPosition[2] };
        const float z = Dot3(rel, camera.cameraForward);
        if (z < MIN_VIEW_DEPTH) {
            return false;
        }
        const float sx = Dot3(rel, camera.cameraRight) / (z * camera.tanHalfFovY * camera.aspectRatio);
        const float sy = Dot3(rel, camera.cameraUp) / (z * camera.tanHalfFovY);
        out.x = origin.x + (sx + 1.0f) * 0.5f * size.x;
        out.y = origin.y + (1.0f - sy) * 0.5f * size.y;
        if (outDepth) {
            *outDepth = z;
        }
        return std::isfinite(out.x) && std::isfinite(out.y);
    }

    float OverlayPixelsPerUnit(const Render::RenderCameraProxy& camera, const ImVec2& size, float depth)
    {
        return 0.5f * size.y / ((depth > MIN_VIEW_DEPTH ? depth : MIN_VIEW_DEPTH) * camera.tanHalfFovY);
    }

    bool IsOccludedBySdf(const Render::Voxel::SignedDistanceGrid& field, const Render::Voxel::VoxelGridDesc& desc,
                         const float cameraPosition[3], const float point[3])
    {
        const float dir[3] = { cameraPosition[0] - point[0], cameraPosition[1] - point[1], cameraPosition[2] - point[2] };
        const float dist = std::sqrt(Dot3(dir, dir));
        const Render::Voxel::SdfTraceSettings settings;
        const Render::Voxel::SdfRayHit hit = Render::Voxel::TraceSdfRay(field, desc, point, dir, settings);
        return hit.hit && hit.t < dist - CAMERA_CLEARANCE * desc.voxelSize;
    }
}
