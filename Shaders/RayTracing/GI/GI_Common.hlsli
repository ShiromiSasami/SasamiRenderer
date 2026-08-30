#ifndef GI_COMMON_HLSLI
#define GI_COMMON_HLSLI

// =============================================================================
// GI_Common.hlsli
// Irradiance Probe Grid — shared HLSL definitions.
//
// Probe data layout: float4[9] per probe  (L2 Spherical Harmonics)
//   .rgb = irradiance SH coefficients
//   .w   = directional hit-distance SH coefficient (visibility signal, baked
//          in GI_ProbeUpdate_CS.hlsl). Bare-basis projection, NOT cosine-lobe
//          convolved — must be reconstructed with GI_EvaluateProbeDistanceSH,
//          never with GI_EvaluateProbeSH.
// Total storage:     9 × 16 = 144 bytes per probe
//
// Probe indexing:  linearIndex = x + y * countX + z * countX * countY
// =============================================================================

#define GI_SH_COEFFICIENT_COUNT 9

// --------------------------------------------------------------------------
// Probe grid constants (b2, space0)
// Must match GIProbeGridCBData in IrradianceProbeGrid.h exactly.
// --------------------------------------------------------------------------
cbuffer GIProbeGridCB : register(b2)
{
    float3 g_probeOrigin;    // World-space corner (minimum) of the grid
    float  g_giIntensity;    // Global GI multiplier
    float3 g_probeSpacing;   // World-space spacing between adjacent probes
    float  g_giEnabled;      // 1.0 = enabled
    uint   g_probeCountX;
    uint   g_probeCountY;
    uint   g_probeCountZ;
    uint   g_probeTotalCount; // countX * countY * countZ
}

// Probe L2 SH data (read-only from PBR_PS; read-write from update CS via separate binding)
StructuredBuffer<float4> g_probeSHData : register(t10);

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

uint GI_ProbeLinearIndex(uint3 coord)
{
    return coord.x + coord.y * g_probeCountX + coord.z * g_probeCountX * g_probeCountY;
}

// Evaluate probe irradiance for a given world-space normal.
// Uses the same cosine-lobe convolution factors as EvaluateDiffuseIrradianceFromSh in PBR_PS.
float3 GI_EvaluateProbeSH(uint probeIdx, float3 n)
{
    uint base = probeIdx * GI_SH_COEFFICIENT_COUNT;
    float x = n.x, y = n.y, z = n.z;

    const float b0 = 0.282095 * 3.14159265;
    const float b1 = (0.488603 * y) * 2.09439510;
    const float b2 = (0.488603 * z) * 2.09439510;
    const float b3 = (0.488603 * x) * 2.09439510;
    const float b4 = (1.092548 * x * y) * 0.78539816;
    const float b5 = (1.092548 * y * z) * 0.78539816;
    const float b6 = (0.315392 * (3.0 * z * z - 1.0)) * 0.78539816;
    const float b7 = (1.092548 * x * z) * 0.78539816;
    const float b8 = (0.546274 * (x * x - y * y)) * 0.78539816;

    float3 irr =
        g_probeSHData[base + 0].rgb * b0 +
        g_probeSHData[base + 1].rgb * b1 +
        g_probeSHData[base + 2].rgb * b2 +
        g_probeSHData[base + 3].rgb * b3 +
        g_probeSHData[base + 4].rgb * b4 +
        g_probeSHData[base + 5].rgb * b5 +
        g_probeSHData[base + 6].rgb * b6 +
        g_probeSHData[base + 7].rgb * b7 +
        g_probeSHData[base + 8].rgb * b8;

    return max(irr, 0.0);
}

// Reconstruct the baked directional hit-distance field for a probe along `dir`.
// Uses the bare (non-cosine-convolved) SH basis constants — the same ones used
// by ProjectOntoSH at bake time. Do NOT reuse GI_EvaluateProbeSH's constants
// here; those are pre-multiplied by Lambertian cosine-lobe convolution factors
// and are only valid for reconstructing irradiance from radiance, not for a
// plain scalar field like distance.
float GI_EvaluateProbeDistanceSH(uint probeIdx, float3 dir)
{
    uint base = probeIdx * GI_SH_COEFFICIENT_COUNT;
    float x = dir.x, y = dir.y, z = dir.z;

    float d =
        g_probeSHData[base + 0].w * 0.282095 +
        g_probeSHData[base + 1].w * (0.488603 * y) +
        g_probeSHData[base + 2].w * (0.488603 * z) +
        g_probeSHData[base + 3].w * (0.488603 * x) +
        g_probeSHData[base + 4].w * (1.092548 * x * y) +
        g_probeSHData[base + 5].w * (1.092548 * y * z) +
        g_probeSHData[base + 6].w * (0.315392 * (3.0 * z * z - 1.0)) +
        g_probeSHData[base + 7].w * (1.092548 * x * z) +
        g_probeSHData[base + 8].w * (0.546274 * (x * x - y * y));

    // An order-2 SH fit of a non-negative function can ring slightly negative
    // near sharp occlusion edges — floor it to a small positive distance.
    return max(d, 0.05);
}

// Soft visibility weight for blending probeIdx (located at probePos) into the
// shading point at worldPos with surface normal `normal`. Combines:
//  - a distance-field occlusion test: if the probe's baked "how far can I see
//    in this direction" estimate is clearly shorter than the actual distance
//    to worldPos, something (floor/ceiling/wall) is probably between them.
//  - a normal-based backface rejection: probes behind the shaded surface
//    (facing away from it) contribute less, reducing bleed through thin slabs
//    even when the distance estimate alone is ambiguous.
float GI_ProbeVisibilityWeight(uint probeIdx, float3 probePos, float3 worldPos, float3 normal)
{
    float3 toPoint = worldPos - probePos;
    float  dist    = length(toPoint);
    if (dist < 1e-4)
        return 1.0;

    float3 dir = toPoint / dist;

    float estDist = GI_EvaluateProbeDistanceSH(probeIdx, dir);

    // Soft-fade band sized as a fraction of the smallest probe cell dimension.
    // This is a tolerance for SH-fit noise, NOT a cell-diagonal margin — too
    // wide a band lets thin occluders (floor/ceiling slabs) go undetected.
    float band = max(min(min(g_probeSpacing.x, g_probeSpacing.y), g_probeSpacing.z) * 0.25, 1e-3);
    float visWeight = smoothstep(estDist + band, estDist - band, dist);

    // Backface rejection: a probe "behind" the shading normal contributes less.
    float normalWeight = saturate(dot(normal, -dir) + 0.5);

    // No 1% floor: a fully-occluded probe must be able to contribute zero.
    // Callers must handle the case where every probe's weight collapses to 0.
    return visWeight * normalWeight;
}

// Sample the probe grid at worldPos for the given surface normal, blending the
// 8 surrounding probes with trilinear weight combined with a per-corner
// visibility weight so probes on the far side of floors/ceilings/walls don't
// bleed their irradiance through occluding geometry.
float3 GI_SampleProbeGrid(float3 worldPos, float3 normal)
{
    if (g_giEnabled < 0.5 || g_probeTotalCount == 0)
        return (float3)0;

    // Push the sample point slightly off the surface along its normal so the
    // surface itself isn't misidentified as an occluder (DDGI-style normal bias).
    const float3 biasedPos = worldPos + normal * (0.25 * min(min(g_probeSpacing.x, g_probeSpacing.y), g_probeSpacing.z));

    float3 local = (biasedPos - g_probeOrigin) / max(g_probeSpacing, float3(1e-6, 1e-6, 1e-6));

    int3 c0 = clamp((int3)floor(local),
                    int3(0, 0, 0),
                    int3((int)g_probeCountX - 1, (int)g_probeCountY - 1, (int)g_probeCountZ - 1));
    int3 c1 = clamp(c0 + int3(1, 1, 1),
                    int3(0, 0, 0),
                    int3((int)g_probeCountX - 1, (int)g_probeCountY - 1, (int)g_probeCountZ - 1));
    float3 t = saturate(local - float3(c0));

    uint3 coords[8] =
    {
        uint3(c0.x, c0.y, c0.z), uint3(c1.x, c0.y, c0.z),
        uint3(c0.x, c1.y, c0.z), uint3(c1.x, c1.y, c0.z),
        uint3(c0.x, c0.y, c1.z), uint3(c1.x, c0.y, c1.z),
        uint3(c0.x, c1.y, c1.z), uint3(c1.x, c1.y, c1.z),
    };

    float trilinearW[8] =
    {
        (1.0 - t.x) * (1.0 - t.y) * (1.0 - t.z),
        t.x         * (1.0 - t.y) * (1.0 - t.z),
        (1.0 - t.x) * t.y         * (1.0 - t.z),
        t.x         * t.y         * (1.0 - t.z),
        (1.0 - t.x) * (1.0 - t.y) * t.z,
        t.x         * (1.0 - t.y) * t.z,
        (1.0 - t.x) * t.y         * t.z,
        t.x         * t.y         * t.z,
    };

    float3 accumIrr = (float3)0;
    float  accumW   = 0.0;

    // Trilinear-only fallback accumulation (ignores visibility) used when the
    // visibility-weighted sum collapses to ~0, e.g. outside the grid or when a
    // point is fully occluded from all 8 surrounding probes.
    float3 fallbackIrr = (float3)0;
    float  fallbackW   = 0.0;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        uint probeIdx = GI_ProbeLinearIndex(coords[i]);
        float3 probePos = g_probeOrigin + float3(coords[i]) * g_probeSpacing;

        float visW = GI_ProbeVisibilityWeight(probeIdx, probePos, biasedPos, normal);
        float w = trilinearW[i] * visW;

        float3 irr = GI_EvaluateProbeSH(probeIdx, normal);
        accumIrr += irr * w;
        accumW   += w;

        fallbackIrr += irr * trilinearW[i];
        fallbackW   += trilinearW[i];
    }

    // Guard against fully-black results when visibility weighting zeroes out
    // every probe: fall back to plain trilinear blending in that case.
    if (accumW < 1e-4)
        return (fallbackIrr / max(fallbackW, 1e-3)) * g_giIntensity;

    return (accumIrr / accumW) * g_giIntensity;
}

#endif // GI_COMMON_HLSLI
