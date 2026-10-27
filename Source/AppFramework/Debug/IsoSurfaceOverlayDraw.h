#pragma once

#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "imgui.h"

#include <cstdint>

namespace SasamiEngine::Render::Fluid
{
    struct IsoSurfaceMesh;
}

namespace SasamiEngine::Render::Voxel
{
    class SignedDistanceGrid;
}

namespace SasamiEngine::App::Debug
{
    // Colour of one vertex from its attribute (e.g. flame age), 0..255 sRGB + alpha 0..1.
    using IsoSurfaceColorFn = void (*)(float attribute, float outRgb[3], float& outAlpha);

    struct IsoSurfaceDrawStyle
    {
        IsoSurfaceColorFn color = nullptr;   // required
        float lightDirection[3] = { 0.0f, 1.0f, 0.0f };  // unit, toward the light
        float ambient = 0.45f;              // shade = ambient + (1 - ambient) * max(N.L, 0) * diffuse
        float diffuse = 1.0f;
        float rim = 0.0f;                   // adds rim * (1 - |N.V|)^2 to the shade (soft silhouettes)
        float emissive = 0.0f;              // 1 = colour unshaded (flames), blended with the shade
        bool cullBackFaces = false;         // translucent surfaces keep both sides
    };

    // Draws an iso-surface into `drawList` (ImGui background list) as per-vertex-coloured triangles
    // (PrimReserve/PrimVtx, Gouraud), sorted back to front by view depth of the centroid because ImGui
    // has no depth buffer. A triangle whose three vertices are all occluded by the SDF
    // (IsOccludedBySdf, sdf may be null = no occlusion) or behind the near limit is skipped.
    // Projection and occlusion run in ParallelFor chunks; the ImGui calls stay on the calling thread.
    // Returns the number of triangles drawn.
    uint32_t DrawIsoSurfaceOverlay(const Render::Fluid::IsoSurfaceMesh& mesh, const IsoSurfaceDrawStyle& style,
                                   const Render::RenderCameraProxy& camera, const ImVec2& origin, const ImVec2& size,
                                   ImDrawList* drawList, const Render::Voxel::SignedDistanceGrid* sdf,
                                   const Render::Voxel::VoxelGridDesc& desc);
}
