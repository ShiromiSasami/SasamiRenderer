#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Toon
{
    // One hand-drawn binary face-shadow mask (1 = lit), row-major, width * height texels.
    struct FaceShadowMask
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> lit;
    };

    // Builds the anime "SDF face shadow" threshold map (Genshin-style): masks drawn for increasing
    // light angles are merged into one grayscale map by interpolating between the signed distance
    // fields of consecutive masks (exact EDT, Felzenszwalb & Huttenlocher 2012).
    //
    // `masks` are ordered by increasing light angle, share one size, number at least 2, and are
    // nested (the lit set of mask k is a subset of the lit set of mask k+1).
    //
    // outThreshold (width * height floats in [0,1]) holds, per texel, the normalized light
    // parameter at which the texel becomes lit, with level k = k / (N - 1):
    //   - lit in mask 0                  -> 0
    //   - first lit in mask k+1          -> (k + a / (a + b)) / (N - 1), strictly between levels k and k+1,
    //     a = distance to the nearest lit texel of mask k, b = distance to the nearest unlit texel of mask k+1
    //     (the fraction is clamped to [1e-4, 1 - 1e-4] so float rounding cannot reach a level)
    //   - never lit                      -> 1 (clamped; the shader treats threshold >= 1 as always shadowed)
    // Mask k is reconstructed exactly by (threshold <= k / (N - 1)) for k < N - 1, and by (threshold < 1) for k = N - 1.
    bool BuildFaceShadowThresholdMap(const std::vector<FaceShadowMask>& masks, std::vector<float>& outThreshold, std::string* error);

    // Lit factor in [0,1]: 1 when lightParam01 >= threshold, smoothed over +-softness. A threshold
    // >= 1 is always shadowed.
    //
    // Shader side (not implemented here): lightParam01 is derived from the angle between the head-forward
    // vector and the light direction projected on the head XZ plane (acos(dot) / PI, 0 = light in front,
    // 1 = light behind). The map is sampled at u when dot(faceRight, L) >= 0 and mirrored (u -> 1 - u)
    // when the light is on the other side (dot(faceRight, L) < 0).
    float SampleFaceShadow(float threshold, float lightParam01, float softness);
}

namespace SasamiEngine { using namespace Render; }
