#ifndef SASAMI_PBR_DIRECT_LIGHTING_HLSLI
#define SASAMI_PBR_DIRECT_LIGHTING_HLSLI

// Point/spot punctual-light accumulation, shared by the deferred
// (DeferredLighting_PS.hlsl) and forward (CookTorranceGGX_PS.hlsl) lighting
// passes. Both paths used to carry their own copy of this loop; keeping a
// single implementation prevents the two from silently drifting apart.
//
// Requires the including shader to have already declared/included, before
// this header:
//   - LightCB.hlsli               (u_lightCounts, u_debugParams, ...)
//   - StructuredBuffer<PointLight> u_pointLights;
//   - StructuredBuffer<SpotLight>  u_spotLights;
#include "Raster/Lighting/PBR/PBR_LightTypes.hlsli"
#include "Raster/Lighting/PBR/PBR_BRDF.hlsli"
#include "Raster/Lighting/PBR/PBR_Shadow.hlsli"

// Cook-Torrance surface parameters needed to evaluate the punctual-light BRDF.
// Filled out identically by the deferred and forward lighting passes.
struct PbrSurface
{
    float3 worldPos;
    float3 N;
    float3 V;
    float3 diffuseReflectance;
    float3 F0;
    float  roughness;
    float  a;   // roughness^2 (GGX alpha)
    float  k;   // Schlick-GGX remapped roughness term: (roughness + 1)^2 / 8
    float  NdotV;
    float  ao;
};

// Accumulates all point-light contributions. Identical math in the deferred and
// forward paths; see PBR_DirectLighting.hlsli for the shared implementation.
float3 AccumulatePointLights(PbrSurface s, float aoDirectStrength)
{
    float3 Lo = 0.0;
    int pointCount = (int)u_lightCounts.x;
    for (int li = 0; li < pointCount; ++li) {
        PointLight pl = u_pointLights[li];
        float3 toL = pl.posRange.xyz - s.worldPos;
        float dist = length(toL);
        if (dist < pl.posRange.w && dist > 1e-4 && pl.colorIntensity.w > 0.0) {
            float3 Lp = toL / dist;
            float atten = saturate(1.0 - dist / max(pl.posRange.w, 1e-3));
            atten *= atten;
            float3 Hp = normalize(s.V + Lp);
            float NdotLp = saturate(dot(s.N, Lp));
            float NdotHp = saturate(dot(s.N, Hp));
            float VdotHp = saturate(dot(s.V, Hp));
            float Dp = DistributionGGX(NdotHp, s.a);
            float Gp = GeometrySmith(s.NdotV, NdotLp, s.k);
            float3 Fp = FresnelSchlick(VdotHp, s.F0);
            float3 specP = (Dp * Gp) * Fp / max(4.0 * s.NdotV * NdotLp, 1e-4);
            float3 diffP = (1.0 - Fp) * s.diffuseReflectance / 3.14159265;
            // Per-light occlusion: screen-space contact shadow toward the light
            // (UE contact shadows / Filament punctual lights) + micro-shadowing.
            float visP = ComputeMicroShadowing(NdotLp, s.ao, aoDirectStrength);
            int pointShadowIndex = (int)pl.params.x;
            if (pointShadowIndex >= 0) {
                visP *= SamplePointShadow(s.worldPos, s.N, pl.posRange.xyz, pointShadowIndex);
            }
            if (NdotLp > 0.0) {
                visP *= ComputePunctualContactShadow(s.worldPos, s.N, Lp, dist);
            }
            Lo += (diffP + specP) * NdotLp * pl.colorIntensity.rgb * pl.colorIntensity.w * atten * visP;
        }
    }
    return Lo;
}

// Accumulates all spot-light contributions (cone attenuation + shadow map).
// Identical math in the deferred and forward paths; see PBR_DirectLighting.hlsli
// for the shared implementation.
float3 AccumulateSpotLights(PbrSurface s, float aoDirectStrength)
{
    float3 Lo = 0.0;
    int spotCount = (int)u_lightCounts.y;
    for (int li = 0; li < spotCount; ++li) {
        SpotLight sl = u_spotLights[li];
        float3 toL = sl.posRange.xyz - s.worldPos;
        float dist = length(toL);
        if (dist < sl.posRange.w && dist > 1e-4 && sl.colorIntensity.w > 0.0) {
            float3 Ls = toL / dist;
            float cosTheta = dot(normalize(-Ls), normalize(sl.dirCosInner.xyz));
            float spot = smoothstep(sl.params.x, sl.dirCosInner.w, cosTheta);
            float atten = saturate(1.0 - dist / max(sl.posRange.w, 1e-3));
            atten *= atten;
            float3 Hs = normalize(s.V + Ls);
            float NdotLs = saturate(dot(s.N, Ls));
            float NdotHs = saturate(dot(s.N, Hs));
            float VdotHs = saturate(dot(s.V, Hs));
            float Ds = DistributionGGX(NdotHs, s.a);
            float Gs = GeometrySmith(s.NdotV, NdotLs, s.k);
            float3 Fs = FresnelSchlick(VdotHs, s.F0);
            float3 specS = (Ds * Gs) * Fs / max(4.0 * s.NdotV * NdotLs, 1e-4);
            float3 diffS = (1.0 - Fs) * s.diffuseReflectance / 3.14159265;

            // Spot shadow: 3x3 PCF, per-light shadow slice from params.y
            float spotShadow = SampleSpotShadow(s.worldPos, s.N, dist, (int)sl.params.y);

            float3 spotColor = sl.colorIntensity.rgb * sl.colorIntensity.w * atten * spot * spotShadow;
            float visS = ComputeMicroShadowing(NdotLs, s.ao, aoDirectStrength);
            if (NdotLs > 0.0) {
                visS *= ComputePunctualContactShadow(s.worldPos, s.N, Ls, dist);
            }
            Lo += (diffS + specS) * NdotLs * spotColor * visS;
        }
    }
    return Lo;
}

#endif // SASAMI_PBR_DIRECT_LIGHTING_HLSLI
