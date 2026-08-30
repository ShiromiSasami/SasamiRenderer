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
    // .x = transparentShellStrength, .y = doubleSided (>0.5 flips back-face normals), .zw unused.
    float4 u_materialTransparencyParams;
}

Texture2D AlbedoTex    : register(t0);
Texture2D OcclusionTex : register(t7);
// Material tangent-space normal map; flat (0.5,0.5,1) fallback when the material has none.
// t17 rather than a low register: the shared raster root signature already covers t0-t16
// (the IBL table alone spans t4-t6).
Texture2D NormalTex    : register(t17);
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

// Perturbs `n` (unit, world space) with a tangent-space normal map sample.
// Vertices carry no tangents, so the TBN is rebuilt from screen-space
// derivatives of position and UV (Schuler, "Normal Mapping Without
// Precomputed Tangents"). Returns `n` unchanged for a degenerate UV frame.
float3 PerturbNormal(float3 n, float3 worldPos, float2 uv)
{
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(dp2, n);
    float3 dp1perp = cross(n, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float maxLen = max(dot(T, T), dot(B, B));
    if (maxLen <= 1e-12) {
        return n;
    }
    float invMax = rsqrt(maxLen);

    float3 tsN;
    tsN.xy = NormalTex.Sample(LinearWrap, uv).xy * 2.0 - 1.0;
    // Reconstruct Z instead of trusting the blue channel: BC5 normal maps carry
    // only two channels and the loader fills blue with a constant.
    tsN.z = sqrt(saturate(1.0 - dot(tsN.xy, tsN.xy)));

    return normalize(T * (invMax * tsN.x) + B * (invMax * tsN.y) + n * tsN.z);
}

PSOutput PSMain(PSInput i, bool isFrontFace : SV_IsFrontFace)
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
    N = PerturbNormal(N, i.worldPos, i.uv);

    // Thin double-sided geometry (awnings, foliage, curtains): flip the back-face
    // normal so it isn't shaded black. Gated per-material so meshes with authoring
    // errors in winding/normals (e.g. Bistro FBX) are never affected unless they
    // explicitly opt in via SurfaceMaterial::doubleSided.
    const bool materialDoubleSided = (u_materialTransparencyParams.y > 0.5);
    if (materialDoubleSided && !isFrontFace) {
        N = -N;
    }
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
