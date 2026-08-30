// DebugProbeGrid_PS.hlsl
// Evaluates L2 SH irradiance for each sphere fragment and outputs the result as colour.
// Provides a "probe grid debug view" matching the style of Unity/Unreal GI visualisation.

cbuffer DebugProbeCB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_worldUnused;
    float4             g_extra0;
    float4             g_extra1;
    float4             g_extra2;
}

#include "RayTracing/GI/GI_Common.hlsli"   // GIProbeGridCB @ b2, g_probeSHData @ t10, GI_EvaluateProbeSH

struct PSInput
{
    float4 svPosition              : SV_POSITION;
    float3 worldN                  : NORMAL;
    nointerpolation uint probeIdx  : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_Target0
{
    float3 n   = normalize(input.worldN);
    float3 irr = GI_EvaluateProbeSH(input.probeIdx, n);

    // Before a bake completes the SH buffer is intentionally zeroed. Show a neutral
    // marker only in that state; baked dark/occluded directions must stay dark.
    if (g_giEnabled < 0.5f || g_probeTotalCount == 0)
    {
        irr = float3(0.8f, 0.8f, 0.8f);
    }

    // Gamma-correct for perceptual display clarity (sRGB approx).
    irr = pow(max(irr, 0.0f), 1.0f / 2.2f);

    return float4(irr, 1.0f);
}
