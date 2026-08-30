#include "Renderer/Culling/Frustum.h"

#include <array>
#include <cmath>

namespace
{
    constexpr int kLeft = 0;
    constexpr int kRight = 1;
    constexpr int kBottom = 2;
    constexpr int kTop = 3;
    constexpr int kNear = 4;
    constexpr int kFar = 5;

    void NormalizePlane(float plane[4])
    {
        const float lenSq = plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2];
        if (lenSq > 0.0f) {
            const float invLen = 1.0f / std::sqrt(lenSq);
            plane[0] *= invLen;
            plane[1] *= invLen;
            plane[2] *= invLen;
            plane[3] *= invLen;
        }
    }

    // Standard "positive vertex" (n-vertex) test: for a plane with normal (a,b,c),
    // the AABB corner most in the direction of the normal is the one most likely to
    // be inside. If even that corner is outside, the whole box is outside.
    bool IsBoxOutsidePlane(const float plane[4], const float boundsMin[3], const float boundsMax[3])
    {
        const float px = (plane[0] >= 0.0f) ? boundsMax[0] : boundsMin[0];
        const float py = (plane[1] >= 0.0f) ? boundsMax[1] : boundsMin[1];
        const float pz = (plane[2] >= 0.0f) ? boundsMax[2] : boundsMin[2];

        const float distance = plane[0] * px + plane[1] * py + plane[2] * pz + plane[3];
        return distance < 0.0f;
    }
}

namespace SasamiRenderer::Culling
{
    FrustumPlanes ExtractFrustumPlanes(const float viewProj[16])
    {
        const float* vp = viewProj;

        // col(j) = { vp[0*4+j], vp[1*4+j], vp[2*4+j], vp[3*4+j] }
        auto col = [vp](int j) -> std::array<float, 4> {
            return { vp[0 * 4 + j], vp[1 * 4 + j], vp[2 * 4 + j], vp[3 * 4 + j] };
        };

        const auto col0 = col(0);
        const auto col1 = col(1);
        const auto col2 = col(2);
        const auto col3 = col(3);

        FrustumPlanes frustum;

        for (int i = 0; i < 4; ++i) {
            frustum.planes[kLeft][i] = col0[i] + col3[i];
            frustum.planes[kRight][i] = col3[i] - col0[i];
            frustum.planes[kBottom][i] = col1[i] + col3[i];
            frustum.planes[kTop][i] = col3[i] - col1[i];
            // D3D depth range is 0..1, so the near plane is simply col(2) (not col(2) + col(3)).
            frustum.planes[kNear][i] = col2[i];
            frustum.planes[kFar][i] = col3[i] - col2[i];
        }

        for (int p = 0; p < 6; ++p) {
            NormalizePlane(frustum.planes[p]);
        }

        return frustum;
    }

    bool IsAabbVisible(const FrustumPlanes& frustum, const float boundsMin[3], const float boundsMax[3])
    {
        for (int p = 0; p < 6; ++p) {
            if (IsBoxOutsidePlane(frustum.planes[p], boundsMin, boundsMax)) {
                return false;
            }
        }
        return true;
    }

    bool IsAabbVisibleIgnoringNearPlane(const FrustumPlanes& frustum, const float boundsMin[3], const float boundsMax[3])
    {
        for (int p = 0; p < 6; ++p) {
            if (p == kNear) {
                continue;
            }
            if (IsBoxOutsidePlane(frustum.planes[p], boundsMin, boundsMax)) {
                return false;
            }
        }
        return true;
    }
}
