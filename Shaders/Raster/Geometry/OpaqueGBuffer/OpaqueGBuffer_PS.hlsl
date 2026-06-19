cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
    float4 u_materialBaseColor;
    float4 u_materialEmissiveRoughness;
    float4 u_materialParams;
    float4 u_materialSpecularWorkflow;
    float4 u_materialReflectionParams;
    float4 u_materialVolumeParams;
    float4 u_materialTransparencyParams;
}

Texture2D AlbedoTex    : register(t0);
Texture2D OcclusionTex : register(t7);
SamplerState LinearWrap : register(s0);

#include "Shared/Common/LightCB.hlsli"
#include "Raster/Lighting/PBR/PBR_BRDF.hlsli"

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

struct PSOutput
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 material : SV_TARGET2;
    float4 emissive : SV_TARGET3;
    float4 specularWorkflow : SV_TARGET4;
};

PSOutput PSMain(PSInput i)
{
    float4 albedoSample = AlbedoTex.Sample(LinearWrap, i.uv);
    float3 albedo = albedoSample.rgb * i.color.rgb * u_materialBaseColor.rgb;
    float materialAlpha = saturate(albedoSample.a * i.color.a * u_materialBaseColor.a);

    const float4 materialTextureSample = OcclusionTex.Sample(LinearWrap, i.uv);
    const float aoSample = saturate(materialTextureSample.r);
    const float occlusionStrength = saturate(u_materialParams.y);
    float materialAo = lerp(1.0, aoSample, occlusionStrength);

    const bool useSpecularGlossiness = (u_materialSpecularWorkflow.w > 0.5);
    float metallic = useSpecularGlossiness ? 0.0 : saturate(u_materialParams.x);
    float roughness = saturate(u_materialEmissiveRoughness.w);
    if (!useSpecularGlossiness && u_materialParams.w > 0.5) {
        roughness = saturate(roughness * materialTextureSample.g);
        metallic = saturate(metallic * materialTextureSample.b);
    }

    float3 N = normalize(i.worldN);
    float3 F0 = useSpecularGlossiness
        ? saturate(u_materialSpecularWorkflow.rgb)
        : lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float authoredReflectionMask = ReflectionMaterialMask(u_materialReflectionParams.x);
    float physicalReflectionMask = useSpecularGlossiness
        ? saturate(max(max(F0.r, F0.g), F0.b) * (1.0 - roughness))
        : saturate(metallic * (1.0 - roughness));

    PSOutput o;
    o.albedo   = float4(saturate(albedo), materialAlpha);
    o.normal   = float4(N * 0.5 + 0.5, length(i.worldPos - u_cameraPos.xyz));
    o.material = float4(roughness, metallic, materialAo, max(authoredReflectionMask, physicalReflectionMask));
    o.emissive = float4(u_materialEmissiveRoughness.rgb, 0.0);
    o.specularWorkflow = float4(saturate(u_materialSpecularWorkflow.rgb), useSpecularGlossiness ? 1.0 : 0.0);
    return o;
}
