//
// SWRT_NRD_Pack_CS.hlsl
// Pre-NRD packing pass: converts ReSTIR shaded output into NRD input format.
// The radiance is the reflection seen from the PRIMARY surface, so normal, roughness and viewZ
// come from the raster GBuffer; only the hit distance comes from the secondary hit.
//
// Bindings (scratch tables):
//   t6  : g_shadedColor     (RGBA16F – reflection radiance from Pass4)
//   t7  : g_secondaryGBuffer(RGBA16F – secondary normal.xyz + reflection hit distance, <0 = miss/invalid)
//   t8  : g_rasterMaterial  (RGBA8   – primary roughness.r, metallic.g, reflection strength.a)
//   t9  : g_rasterNormal    (RGBA16F – primary N*0.5+0.5 in xyz, camera distance in .w)
//   u0  : g_nrdSpecIn       (RGBA16F – OUT: demodulated radiance + hitDist for RELAX)
//   u1  : g_nrdViewZ        (R16F    – OUT: primary linear view-space depth)
//   u2  : g_nrdNormalRoughness (R10G10B10A2 – OUT: NRD packed primary normal+roughness)
//
// TODO(NRD demodulation): the consumer of the denoised result (copy to outTexture in
// GpuSoftwareRayTracerReSTIR.cpp) does not multiply SpecularDemodulationFactor back in yet.
//

#include "Denoising/NRD/NRDConfig.hlsli"
#include "Denoising/NRD/NRD.hlsli"

cbuffer ReSTIRFrameConstants : register(b0)
{
    row_major float4x4 g_invVP;
    row_major float4x4 g_prevVP;
    float3 g_cameraPos;
    float  g_tMin;
    uint   g_renderWidth;
    uint   g_renderHeight;
    uint   g_frameIndex;
    uint   g_reservoirWidth;
    float  g_temporalAlpha;
    float  g_phiColor;
    float  g_phiNormal;
    float  g_phiDepth;
    float  g_stepWidth;
    float  g_maxSurfaceRoughness;
    float  g_maxPrimaryHitDistance;
    float  g_minReflectionEnergy;
    float3 g_dirLightDir;
    float  g_dirLightIntensity;
    float3 g_dirLightColor;
    float  g_microShadowStrength;
    float3 g_ambientColor;
    float  g_ambientIntensity;
    uint   g_pointLightCount;
    uint   g_spotLightCount;
    uint   g_gbufferWidth;
    uint   g_gbufferHeight;
    uint   g_punctualLightFalloff; // RendererEnums::PunctualLightFalloff (offset 256)
};

Texture2D<float4>    g_shadedColor        : register(t6);
Texture2D<float4>    g_secondaryGBuffer   : register(t7);
Texture2D<float4>    g_rasterMaterial     : register(t8);
Texture2D<float4>    g_rasterNormal       : register(t9);

RWTexture2D<float4>  g_nrdSpecIn          : register(u0);
RWTexture2D<float>   g_nrdViewZ           : register(u1);
RWTexture2D<float4>  g_nrdNormalRoughness : register(u2);

static const float kNrdSkyViewZ = 65504.0f;

float3 UnprojectToWorld(float ndcX, float ndcY)
{
    // Reverse-Z: far plane is NDC z == 0.
    float4 p = mul(float4(ndcX, ndcY, 0.0f, 1.0f), g_invVP);
    return p.xyz / p.w;
}

// Analytic split-sum environment BRDF (Karis 2014, mobile approximation).
float3 SpecularDemodulationFactor(float3 F0, float NoV, float roughness)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return max(F0 * AB.x + AB.y, 0.01f);
}

[numthreads(16, 16, 1)]
void CS_NRD_Pack(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    uint2 coord = id.xy;
    uint2 gbufSize = uint2(max(g_gbufferWidth, 1u), max(g_gbufferHeight, 1u));
    uint2 gbufPixel = uint2(
        min(uint(float(coord.x) * float(gbufSize.x) / float(g_renderWidth)),  gbufSize.x - 1u),
        min(uint(float(coord.y) * float(gbufSize.y) / float(g_renderHeight)), gbufSize.y - 1u));

    float4 shaded     = g_shadedColor[coord];
    float4 secondary  = g_secondaryGBuffer[coord];
    float4 rasterN    = g_rasterNormal.Load(int3(gbufPixel, 0));
    float4 rasterMat  = g_rasterMaterial.Load(int3(gbufPixel, 0));

    float primaryDistance = rasterN.w;
    if (primaryDistance <= 0.0f)
    {
        g_nrdSpecIn[coord] = RELAX_FrontEnd_PackRadianceAndHitDist(float3(0.0f, 0.0f, 0.0f), 0.0f, true);
        g_nrdViewZ[coord] = kNrdSkyViewZ;
        g_nrdNormalRoughness[coord] = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0f, 0.0f, 1.0f), 1.0f, 0);
        return;
    }

    float3 normalWS = normalize(rasterN.xyz * 2.0f - 1.0f);
    float roughness = saturate(rasterMat.r);
    float metallic  = saturate(rasterMat.g);

    float ndcX =  ((float(gbufPixel.x) + 0.5f) / float(gbufSize.x)) * 2.0f - 1.0f;
    float ndcY = -((float(gbufPixel.y) + 0.5f) / float(gbufSize.y)) * 2.0f + 1.0f;
    float3 rayDir  = normalize(UnprojectToWorld(ndcX, ndcY) - g_cameraPos);
    float3 forward = normalize(UnprojectToWorld(0.0f, 0.0f) - g_cameraPos);
    float viewZ = primaryDistance * dot(rayDir, forward);

    float3 V = -rayDir;
    if (dot(normalWS, V) < 0.0f) normalWS = -normalWS;
    float NoV = saturate(dot(normalWS, V));

    // Raw hit distance for RELAX; a miss travelled the full reflection range.
    float hitDist = (secondary.w > 0.0f) ? secondary.w : g_maxPrimaryHitDistance;

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), float3(0.5f, 0.5f, 0.5f), metallic);
    float3 demodulated = shaded.rgb / SpecularDemodulationFactor(F0, NoV, roughness);

    g_nrdSpecIn[coord] = RELAX_FrontEnd_PackRadianceAndHitDist(demodulated, hitDist, true);
    g_nrdViewZ[coord] = viewZ;
    g_nrdNormalRoughness[coord] = NRD_FrontEnd_PackNormalAndRoughness(normalWS, roughness, 0);
}
