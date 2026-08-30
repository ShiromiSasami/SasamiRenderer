#include "Shared/Common/LightCB.hlsli"

Texture2D<float4> GBufferAlbedoTex   : register(t0);
Texture2D<float4> GBufferNormalTex   : register(t1);
Texture2D<float4> GBufferMaterialTex : register(t2);
Texture2D<float4> GBufferEmissiveTex : register(t3);
Texture2DArray ShadowMapTex          : register(t4);
Texture2D SceneDepthTex              : register(t5);
Texture2D RuntimeAOTex               : register(t9);
Texture2D ReflectionTex              : register(t11);
// Shares the deferred lighting root signature (psoGBufferDebug = psoDeferredLighting),
// so t12 carries the spot shadow map here exactly as it does in DeferredLighting_PS.
Texture2DArray SpotShadowMapTex      : register(t12);
Texture2DArray<float2> ShadowVSMTex  : register(t13);
Texture2DArray PointShadowMapTex     : register(t16);
SamplerState LinearWrap             : register(s0);

// s1: depth comparison sampler for shadow PCF (clamped, LESS_EQUAL). See MakeShadowComparisonSampler.
SamplerComparisonState ShadowCmp : register(s1);
#include "Raster/Lighting/PBR/PBR_Shadow.hlsli"

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

#define u_cameraInvPV u_invCameraPV
#include "Shared/Common/WorldPosReconstruction.hlsli"
#undef u_cameraInvPV

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 screenSize = max(u_reflectionParams.zw, float2(1.0, 1.0));
    uint2 pixel = uint2(min(floor(input.pos.xy), screenSize - 1.0));
    float2 uv = saturate((float2(pixel) + 0.5) / screenSize);

    float depth = SceneDepthTex.Load(int3(pixel, 0)).r;
    float4 normalSample = GBufferNormalTex.Load(int3(pixel, 0));
    if (depth >= 1.0 || normalSample.w <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 albedo = saturate(GBufferAlbedoTex.Load(int3(pixel, 0)).rgb);
    float3 N = normalize(normalSample.xyz * 2.0 - 1.0);
    float4 material = GBufferMaterialTex.Load(int3(pixel, 0));
    float roughness = saturate(material.r);
    float metallic = saturate(material.g);
    float materialAo = saturate(material.b);
    float3 emissiveColor = GBufferEmissiveTex.Load(int3(pixel, 0)).rgb;

    float runtimeAo = 1.0;
    if (u_reflectionParams.z > 0.5 && u_reflectionParams.w > 0.5) {
        runtimeAo = RuntimeAOTex.SampleLevel(LinearWrap, uv, 0).r;
    }
    float rawAo = saturate(materialAo * runtimeAo);
    float ao = lerp(saturate(u_shadowParams.w), 1.0, rawAo);

    float3 Ld = normalize(-u_dirDir.xyz);
    float NdotL = saturate(dot(N, Ld));
    float directionalVisibility = 0.0;
    if (NdotL > 0.0) {
        float3 worldPos = ReconstructWorldPos(uv, depth);
        directionalVisibility = ShadowVisibility(worldPos, N, Ld, NdotL) *
            ComputeDirectionalContactShadow(worldPos, N, Ld);
    }

    const int debugMode = (int)(u_debugParams.x + 0.5);
    if (debugMode == 1) return float4(albedo, 1.0);
    if (debugMode == 2) return float4(N * 0.5 + 0.5, 1.0);
    if (debugMode == 3) return float4(roughness.xxx, 1.0);
    if (debugMode == 4) return float4(metallic.xxx, 1.0);
    if (debugMode == 5) return float4(ao.xxx, 1.0);
    if (debugMode == 6) return float4(directionalVisibility.xxx, 1.0);
    if (debugMode == 7) return float4(emissiveColor, 1.0);
    if (debugMode == 8 || debugMode == 9) return float4(runtimeAo.xxx, 1.0);
    if (debugMode == 10) return float4(Ld * 0.5 + 0.5, 1.0);
    if (debugMode == 11) return float4(NdotL.xxx, 1.0);
    if (debugMode == 12) return float4(saturate(ReflectionTex.SampleLevel(LinearWrap, uv, 0).rgb), 1.0);
    if (debugMode == 13) return float4(saturate(ReflectionTex.SampleLevel(LinearWrap, uv, 0).a).xxx, 1.0);
    if (debugMode == 14) return float4(saturate(ReflectionTex.SampleLevel(LinearWrap, uv, 0).r).xxx, 1.0);

    // SwrtReflectionComposite is written by the later reflection composite pass.
    return float4(0.0, 0.0, 0.0, 1.0);
}
