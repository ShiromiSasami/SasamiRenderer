#include "Shared/Common/LightCB.hlsli"
#include "Raster/Lighting/PBR/PBR_LightTypes.hlsli"

Texture2D<float4> GBufferAlbedoTex   : register(t0);
Texture2D<float4> GBufferNormalTex   : register(t1);
Texture2D<float4> GBufferMaterialTex : register(t2);
Texture2D<float4> GBufferEmissiveTex : register(t3);
Texture2DArray ShadowMapTex          : register(t4);
Texture2D SceneDepthTex              : register(t5);
StructuredBuffer<PointLight> u_pointLights : register(t6);
StructuredBuffer<SpotLight> u_spotLights   : register(t7);
TextureCube IrradianceTex            : register(t8);
Texture2D RuntimeAOTex               : register(t9);
Texture2DArray SpotShadowMapTex      : register(t12);
Texture2DArray PointShadowMapTex     : register(t16);
Texture2DArray<float2> ShadowVSMTex  : register(t13);
Texture2D<float> TransparentBackfaceDistanceTex : register(t14);
Texture2D<float4> GBufferSpecularWorkflowTex : register(t15);
TextureCube IblPrefilterTex          : register(t17);
Texture2D IblBrdfLutTex              : register(t18);
SamplerState LinearWrap : register(s0);

// s1: depth comparison sampler for shadow PCF (clamped, LESS_EQUAL). See MakeShadowComparisonSampler.
SamplerComparisonState ShadowCmp : register(s1);
#include "Raster/Lighting/PBR/PBR_BRDF.hlsli"
#include "Raster/Lighting/PBR/PBR_IBL.hlsli"
#include "Raster/Lighting/PBR/PBR_Shadow.hlsli"
#include "Raster/Lighting/PBR/PBR_DirectLighting.hlsli"
#include "RayTracing/GI/GI_Common.hlsli"

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

#define u_cameraInvPV u_invCameraPV
#include "Shared/Common/WorldPosReconstruction.hlsli"
#undef u_cameraInvPV

// Split-sum specular IBL (Karis 2013, "Real Shading in Unreal Engine 4"):
// the prefiltered radiance cube supplies the pre-integrated lighting term and the
// 2D LUT supplies the BRDF term, so a mirror-like environment response is possible
// where the L2 SH probes used for diffuse GI cannot represent one.
float3 EvaluateSpecularIbl(float3 N, float3 V, float3 F0, float roughness, float specularOcclusion)
{
    if (u_iblParams.x <= 0.5) { return (float3)0; }
    const float NdotV = saturate(dot(N, V));
    const float3 R = reflect(-V, N);
    const float mip = saturate(roughness) * max(u_iblParams.z, 0.0);
    const float3 prefiltered = IblPrefilterTex.SampleLevel(LinearWrap, R, mip).rgb;
    const float2 ab = IblBrdfLutTex.SampleLevel(LinearWrap, saturate(float2(NdotV, roughness)), 0).rg;
    return prefiltered * (F0 * ab.x + ab.y) * specularOcclusion * u_iblParams.y;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 screenSize = max(u_reflectionParams.zw, float2(1.0, 1.0));
    uint2 pixel = uint2(min(floor(input.pos.xy), screenSize - 1.0));
    float2 uv = saturate((float2(pixel) + 0.5) / screenSize);

    float depth = SceneDepthTex.Load(int3(pixel, 0)).r;
    float4 normalSample = GBufferNormalTex.Load(int3(pixel, 0));
    float cameraDistance = normalSample.w;
    if (depth >= 1.0 || cameraDistance <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float4 albedoSample = GBufferAlbedoTex.Load(int3(pixel, 0));
    float4 material = GBufferMaterialTex.Load(int3(pixel, 0));
    float3 emissiveColor = GBufferEmissiveTex.Load(int3(pixel, 0)).rgb;
    float4 specularWorkflow = GBufferSpecularWorkflowTex.Load(int3(pixel, 0));

    float3 albedo = saturate(albedoSample.rgb);
    float materialAlpha = saturate(albedoSample.a);
    float3 N = normalize(normalSample.xyz * 2.0 - 1.0);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 V = normalize(u_cameraPos.xyz - worldPos);
    float NdotV = saturate(dot(N, V));

    float roughness = saturate(material.r);
    float metallic = saturate(material.g);
    float materialAo = saturate(material.b);
    float materialReflectionMask = saturate(material.a);
    bool useSpecularGlossiness = (specularWorkflow.a > 0.5);
    float3 specularColor = saturate(specularWorkflow.rgb);

    float runtimeAo = 1.0;
    if (u_reflectionParams.z > 0.5 && u_reflectionParams.w > 0.5) {
        runtimeAo = RuntimeAOTex.SampleLevel(LinearWrap, uv, 0).r;
    }
    float rawAo = saturate(materialAo * runtimeAo);
    float ao = lerp(saturate(u_shadowParams.w), 1.0, rawAo);
    float iblVisibility = rawAo;

    float3 F0 = useSpecularGlossiness
        ? specularColor
        : lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float specularEnergy = saturate(max(max(F0.r, F0.g), F0.b));
    float3 diffuseReflectance = useSpecularGlossiness
        ? albedo * (1.0 - specularEnergy)
        : albedo * (1.0 - metallic);

    float3 Lo = 0.0;
    float3 Ld = normalize(-u_dirDir.xyz);
    float3 H = normalize(V + Ld);
    float NdotL = saturate(dot(N, Ld));
    float directionalVisibility = 0.0;
    if (NdotL > 0.0) {
        float vis = ShadowVisibility(worldPos, N, Ld, NdotL);
        float contactVis = ComputeDirectionalContactShadow(worldPos, N, Ld);
        directionalVisibility = vis * contactVis;
    }

    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float a = roughness * roughness;
    float D = DistributionGGX(NdotH, a);
    float k = (roughness + 1.0);
    k = (k * k) / 8.0;
    float G = GeometrySmith(NdotV, NdotL, k);
    float3 F = FresnelSchlick(VdotH, F0);
    float3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    float3 diffuse = (1.0 - F) * diffuseReflectance / 3.14159265;
    // Micro-shadowing on top of shadow-map visibility (Chan 2018, adopted by Filament).
    float dirMicroShadow = ComputeMicroShadowing(NdotL, ao, u_debugParams.y);
    Lo += (diffuse + spec) * NdotL * (u_dirColor.rgb * u_dirDir.w) * directionalVisibility * dirMicroShadow;

    // Point/spot direct lighting: identical math to the forward path (CookTorranceGGX_PS.hlsl).
    // See PBR_DirectLighting.hlsli for the shared implementation.
    PbrSurface surf;
    surf.worldPos = worldPos;
    surf.N = N;
    surf.V = V;
    surf.diffuseReflectance = diffuseReflectance;
    surf.F0 = F0;
    surf.roughness = roughness;
    surf.a = a;
    surf.k = k;
    surf.NdotV = NdotV;
    surf.ao = ao;
    Lo += AccumulatePointLights(surf, u_debugParams.y);
    Lo += AccumulateSpotLights(surf, u_debugParams.y);

    float3 indirectDiffuse = 0.0;
    float3 Fibl = FresnelSchlick(NdotV, F0);
    float3 kdIbl = (1.0 - Fibl);
    if (g_giEnabled > 0.5) {
        // Lambertian BRDF: outgoing radiance is E * albedo / PI. GI_SampleProbeGrid returns
        // irradiance E -- its SH is convolved with the cosine lobe (A0=PI, A1=2PI/3,
        // A2=PI/4) -- so the 1/PI still belongs here, exactly like the direct-light
        // diffuse term above. Without it the indirect diffuse is PI times too bright,
        // which reads as up-facing surfaces (floors, roofs) being blown out.
        float3 probeIrradiance = GI_SampleProbeGrid(worldPos, N);
        indirectDiffuse = kdIbl * (probeIrradiance / 3.14159265) * diffuseReflectance * iblVisibility;
    } else if (u_iblParams.x > 0.5) {
        // EvaluateDiffuseIrradianceFromSh returns irradiance E (same cosine-lobe
        // convolution as the probe SH) and needs the Lambert 1/PI. The prefiltered
        // cubemap instead stores a cosine-weighted average radiance (E/PI already),
        // so only the SH variant is divided here.
        float3 iblDiffuse = (u_iblParams.w > 0.5)
            ? EvaluateDiffuseIrradianceFromSh(N) / 3.14159265
            : IrradianceTex.Sample(LinearWrap, N).rgb;
        indirectDiffuse = kdIbl * iblDiffuse * diffuseReflectance * iblVisibility * u_iblParams.y;
    }

    // Specular occlusion reuses the existing indirect-visibility term (iblVisibility);
    // SpecularOcclusion (PBR_BRDF.hlsli) relaxes it toward NdotV/roughness so occluded
    // metals do not lose their reflection entirely.
    float specularOcclusion = SpecularOcclusion(iblVisibility, NdotV, roughness);
    float3 indirectSpecular = EvaluateSpecularIbl(N, V, F0, roughness, specularOcclusion);

    float3 color = indirectDiffuse + indirectSpecular + Lo + emissiveColor * ao + 0.01 * albedo * ao;

    return float4(color, materialAlpha);
}
