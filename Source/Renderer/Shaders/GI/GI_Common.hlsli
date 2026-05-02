#ifndef GI_COMMON_HLSLI
#define GI_COMMON_HLSLI

// =============================================================================
// GI_Common.hlsli
// Irradiance Probe Grid — shared HLSL definitions.
//
// Probe data layout: float4[9] per probe  (L2 Spherical Harmonics, xyz=RGB, w=0)
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

// Trilinearly sample the probe grid at worldPos for the given surface normal.
float3 GI_SampleProbeGrid(float3 worldPos, float3 normal)
{
    if (g_giEnabled < 0.5 || g_probeTotalCount == 0)
        return (float3)0;

    float3 local = (worldPos - g_probeOrigin) / max(g_probeSpacing, float3(1e-6, 1e-6, 1e-6));

    int3 c0 = clamp((int3)floor(local),
                    int3(0, 0, 0),
                    int3((int)g_probeCountX - 1, (int)g_probeCountY - 1, (int)g_probeCountZ - 1));
    int3 c1 = clamp(c0 + int3(1, 1, 1),
                    int3(0, 0, 0),
                    int3((int)g_probeCountX - 1, (int)g_probeCountY - 1, (int)g_probeCountZ - 1));
    float3 t = saturate(local - float3(c0));

    float3 i000 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c0.x, c0.y, c0.z)), normal);
    float3 i100 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c1.x, c0.y, c0.z)), normal);
    float3 i010 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c0.x, c1.y, c0.z)), normal);
    float3 i110 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c1.x, c1.y, c0.z)), normal);
    float3 i001 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c0.x, c0.y, c1.z)), normal);
    float3 i101 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c1.x, c0.y, c1.z)), normal);
    float3 i011 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c0.x, c1.y, c1.z)), normal);
    float3 i111 = GI_EvaluateProbeSH(GI_ProbeLinearIndex(uint3(c1.x, c1.y, c1.z)), normal);

    float3 ix00 = lerp(i000, i100, t.x);
    float3 ix10 = lerp(i010, i110, t.x);
    float3 ix01 = lerp(i001, i101, t.x);
    float3 ix11 = lerp(i011, i111, t.x);
    float3 ixy0 = lerp(ix00, ix10, t.y);
    float3 ixy1 = lerp(ix01, ix11, t.y);

    return lerp(ixy0, ixy1, t.z) * g_giIntensity;
}

#endif // GI_COMMON_HLSLI
