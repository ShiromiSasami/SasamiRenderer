//
// SWRT_NRD_Pack_CS.hlsl
// Pre-NRD packing pass: converts ReSTIR shaded output into NRD input format.
//
// Bindings (scratch tables):
//   t6  : g_shadedColor  (RGBA16F – shaded radiance from Pass4)
//   t7  : g_gbuffer      (RGBA16F – normal.xyz in [0,1] + linearDepth in .w)
//   t8  : g_material     (RGBA8   – roughness.r, metallic.g)
//   u0  : g_nrdDiffIn    (RGBA16F – OUT: packed radiance + hitDist for RELAX)
//   u1  : g_nrdViewZ     (R16F    – OUT: linear view-space depth)
//   u2  : g_nrdNormalRoughness (RGBA8 – OUT: NRD packed normal+roughness)
//

#include "Denoising/NRD/NRDConfig.hlsli"
#include "Denoising/NRD/NRD.hlsli"

Texture2D<float4>    g_shadedColor        : register(t6);
Texture2D<float4>    g_gbuffer            : register(t7);
Texture2D<float4>    g_material           : register(t8);

RWTexture2D<float4>  g_nrdDiffIn          : register(u0);
RWTexture2D<float>   g_nrdViewZ           : register(u1);
RWTexture2D<float4>  g_nrdNormalRoughness : register(u2);

[numthreads(16, 16, 1)]
void CS_NRD_Pack(uint3 id : SV_DispatchThreadID)
{
    uint2 coord = id.xy;

    float4 shaded   = g_shadedColor[coord];
    float4 gbuf     = g_gbuffer[coord];
    float4 material = g_material[coord];

    float3 normalWS = normalize(gbuf.xyz);

    float linearDepth = gbuf.w;
    float roughness   = material.a;

    // Pack radiance + hitDist for RELAX diffuse input.
    // hitDist = 1.0 is a placeholder (we have no per-pixel hit distance).
    float4 packed = RELAX_FrontEnd_PackRadianceAndHitDist(shaded.rgb, 1.0, true);
    g_nrdDiffIn[coord] = packed;

    // Linear depth as viewZ (positive = in front of camera)
    g_nrdViewZ[coord] = linearDepth;

    // Pack normal + roughness in NRD format
    g_nrdNormalRoughness[coord] = NRD_FrontEnd_PackNormalAndRoughness(normalWS, roughness, 0);
}
