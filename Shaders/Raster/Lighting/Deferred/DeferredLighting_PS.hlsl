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
Texture2D ReflectionTex              : register(t11);
Texture2D SpotShadowMapTex           : register(t12);
Texture2DArray<float2> ShadowVSMTex  : register(t13);
Texture2D<float> TransparentBackfaceDistanceTex : register(t14);
Texture2D<float4> GBufferSpecularWorkflowTex : register(t15);
SamplerState LinearWrap : register(s0);

#include "Raster/Lighting/PBR/PBR_BRDF.hlsli"
#include "Raster/Lighting/PBR/PBR_IBL.hlsli"
#include "Raster/Lighting/PBR/PBR_Shadow.hlsli"
#include "RayTracing/GI/GI_Common.hlsli"

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 worldH = mul(float4(ndc, depth, 1.0), u_invCameraPV);
    return worldH.xyz / max(worldH.w, 1e-6);
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
    float3 worldPos = ReconstructWorldPosition(uv, depth);
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
    Lo += (diffuse + spec) * NdotL * (u_dirColor.rgb * u_dirDir.w) * directionalVisibility;

    int pointCount = (int)u_lightCounts.x;
    for (int li = 0; li < pointCount; ++li) {
        PointLight pl = u_pointLights[li];
        float3 toL = pl.posRange.xyz - worldPos;
        float dist = length(toL);
        if (dist < pl.posRange.w && dist > 1e-4 && pl.colorIntensity.w > 0.0) {
            float3 Lp = toL / dist;
            float atten = saturate(1.0 - dist / max(pl.posRange.w, 1e-3));
            atten *= atten;
            float3 Hp = normalize(V + Lp);
            float NdotLp = saturate(dot(N, Lp));
            float NdotHp = saturate(dot(N, Hp));
            float VdotHp = saturate(dot(V, Hp));
            float Dp = DistributionGGX(NdotHp, a);
            float Gp = GeometrySmith(NdotV, NdotLp, k);
            float3 Fp = FresnelSchlick(VdotHp, F0);
            float3 specP = (Dp * Gp) * Fp / max(4.0 * NdotV * NdotLp, 1e-4);
            float3 diffP = (1.0 - Fp) * diffuseReflectance / 3.14159265;
            Lo += (diffP + specP) * NdotLp * pl.colorIntensity.rgb * pl.colorIntensity.w * atten;
        }
    }

    int spotCount = (int)u_lightCounts.y;
    for (int li = 0; li < spotCount; ++li) {
        SpotLight sl = u_spotLights[li];
        float3 toL = sl.posRange.xyz - worldPos;
        float dist = length(toL);
        if (dist < sl.posRange.w && dist > 1e-4 && sl.colorIntensity.w > 0.0) {
            float3 Ls = toL / dist;
            float cosTheta = dot(normalize(-Ls), normalize(sl.dirCosInner.xyz));
            float spot = smoothstep(sl.params.x, sl.dirCosInner.w, cosTheta);
            float atten = saturate(1.0 - dist / max(sl.posRange.w, 1e-3));
            atten *= atten;
            float3 Hs = normalize(V + Ls);
            float NdotLs = saturate(dot(N, Ls));
            float NdotHs = saturate(dot(N, Hs));
            float VdotHs = saturate(dot(V, Hs));
            float Ds = DistributionGGX(NdotHs, a);
            float Gs = GeometrySmith(NdotV, NdotLs, k);
            float3 Fs = FresnelSchlick(VdotHs, F0);
            float3 specS = (Ds * Gs) * Fs / max(4.0 * NdotV * NdotLs, 1e-4);
            float3 diffS = (1.0 - Fs) * diffuseReflectance / 3.14159265;
            float spotShadow = 1.0;
            if (li == 0 && u_spotShadowParams.z > 0.5) {
                float4 sc = mul(float4(worldPos, 1.0), u_spotLightVP);
                sc.xyz /= sc.w;
                float2 shadowUV = sc.xy * 0.5 + 0.5;
                shadowUV.y = 1.0 - shadowUV.y;
                float shadowDepth = sc.z - u_spotShadowParams.x;
                if (all(shadowUV >= 0.0) && all(shadowUV <= 1.0) && sc.w > 0.0) {
                    float texelSize = 1.0 / u_spotShadowParams.w;
                    float shadowSum = 0.0;
                    [unroll] for (int sy = -1; sy <= 1; ++sy)
                    [unroll] for (int sx = -1; sx <= 1; ++sx) {
                        float sampleD = SpotShadowMapTex.Sample(LinearWrap, shadowUV + float2(sx, sy) * texelSize).r;
                        shadowSum += (sampleD >= shadowDepth) ? 1.0 : 0.0;
                    }
                    spotShadow = shadowSum / 9.0;
                }
            }
            Lo += (diffS + specS) * NdotLs * sl.colorIntensity.rgb * sl.colorIntensity.w * atten * spot * spotShadow;
        }
    }

    float3 indirectDiffuse = 0.0;
    if (u_iblParams.x > 0.5) {
        float3 Fibl = FresnelSchlick(NdotV, F0);
        float3 kdIbl = (1.0 - Fibl);
        float3 probeIrradiance = GI_SampleProbeGrid(worldPos, N);
        float3 iblIrradiance = (u_iblParams.w > 0.5)
            ? EvaluateDiffuseIrradianceFromSh(N)
            : IrradianceTex.Sample(LinearWrap, N).rgb;
        float3 irradiance = (g_giEnabled > 0.5) ? probeIrradiance : iblIrradiance;
        indirectDiffuse = kdIbl * irradiance * diffuseReflectance * iblVisibility * u_iblParams.y;
    }

    float3 color = indirectDiffuse + Lo + emissiveColor + 0.01 * albedo * ao;

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
    if (debugMode == 15) return float4(0.0, 0.0, 0.0, 1.0);

    return float4(color, materialAlpha);
}
