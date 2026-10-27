#pragma once

#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "imgui.h"

namespace SasamiEngine::Render::Voxel
{
    class SignedDistanceGrid;
}

namespace SasamiEngine::App::Debug
{
    // Pinhole projection of a world point into ImGui display pixels (same model as WindParticleImage).
    // Fails behind the near limit. outDepth, when given, receives the view depth (world units).
    bool ProjectToOverlay(const Render::RenderCameraProxy& camera, const ImVec2& origin, const ImVec2& size,
                          const float point[3], ImVec2& out, float* outDepth = nullptr);

    // Display pixels covered by one world unit at view depth `depth`.
    float OverlayPixelsPerUnit(const Render::RenderCameraProxy& camera, const ImVec2& size, float depth);

    // True when solid voxels lie between `point` and the camera. The ray is traced from the point
    // toward the camera, ignoring solids within 2 voxels of the camera: traced from a camera standing
    // inside solid voxels (e.g. at floor level) it would hit on its first step and hide everything.
    // Voxel-accurate only; there is no depth-buffer test.
    bool IsOccludedBySdf(const Render::Voxel::SignedDistanceGrid& field, const Render::Voxel::VoxelGridDesc& desc,
                         const float cameraPosition[3], const float point[3]);
}
