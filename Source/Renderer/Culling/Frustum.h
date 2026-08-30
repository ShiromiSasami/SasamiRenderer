#pragma once

namespace SasamiRenderer::Culling
{
    // Row-major, row-vector view-projection matrices (clip = p * VP, D3D 0..1 depth).
    // Plane extraction follows Gribb-Hartmann against VP's columns.
    struct FrustumPlanes
    {
        // (a, b, c, d) per plane, normalized. Inside when a*x + b*y + c*z + d >= 0.
        float planes[6][4] = {};
    };

    // viewProj: row-major, row-vector, D3D 0..1 depth.
    FrustumPlanes ExtractFrustumPlanes(const float viewProj[16]);

    // Conservative AABB test: only returns false when the box is provably outside the frustum.
    // Boxes straddling a plane are treated as visible (false positives are cheap, false
    // negatives make objects disappear and must never happen).
    bool IsAabbVisible(const FrustumPlanes& frustum, const float boundsMin[3], const float boundsMax[3]);

    // Same conservative test as IsAabbVisible, but the near plane is skipped.
    // Intended for shadow cascade culling: an object outside a directional light's ortho
    // box but sitting between the light and the box can still cast a shadow into the box.
    // Culling it against the near plane would pop that shadow off-screen, so the near
    // plane is intentionally left unchecked here.
    bool IsAabbVisibleIgnoringNearPlane(const FrustumPlanes& frustum, const float boundsMin[3], const float boundsMax[3]);
}
