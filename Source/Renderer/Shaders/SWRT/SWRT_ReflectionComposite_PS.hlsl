#include "Common/LightCB.hlsli"

// GBuffer inputs
Texture2D<float4> GBufferAlbedoTex   : register(t0);
Texture2D<float4> GBufferMaterialTex : register(t7);
Texture2D<float4> ReflectionTex      : register(t8);
Texture2D<float4> GBufferNormalTex   : register(t9);
SamplerState LinearWrap         : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 CompositeFresnelSchlick(float cosTheta, float3 F0)
{
    float f = pow(saturate(1.0 - cosTheta), 5.0);
    return F0 + (1.0 - F0) * f;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    int debugMode = (int)(u_debugParams.x + 0.5);
    if (debugMode != 0 && debugMode != 15)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float2 screenSize = max(u_reflectionParams.zw, float2(1.0, 1.0));
    uint2 pixel = uint2(min(floor(input.pos.xy), screenSize - 1.0));
    float2 pixelCenter = float2(pixel) + 0.5;
    float2 uv = saturate(pixelCenter / screenSize);

    // Use exact raster GBuffer texels for composition. Linear filtering blends
    // material masks/normals across object edges and causes reflection halos.
    float3 albedo   = saturate(GBufferAlbedoTex.Load(int3(pixel, 0)).rgb);
    float4 material = GBufferMaterialTex.Load(int3(pixel, 0));
    float4 reflSamp = ReflectionTex.SampleLevel(LinearWrap, uv, 0);
    float4 normSamp = GBufferNormalTex.Load(int3(pixel, 0));
    float  cameraDistance = normSamp.w;

    float roughness              = saturate(material.r);
    float metallic               = saturate(material.g);
    float materialReflectionMask = saturate(material.a);

    // No specular contribution for surfaces that opt out of reflections.
    if (cameraDistance <= 0.0 || materialReflectionMask <= 0.001)
        return float4(0.0, 0.0, 0.0, 0.0);

    // Match SWRT_Reflection_CS.hlsl: reconstruct the camera ray from the same
    // pixel center, then use GBufferNormal.w as camera-to-surface distance.
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 viewRayH = mul(float4(ndc, 1.0, 1.0), u_invCameraPV);
    float3 rayDir = normalize(viewRayH.xyz / viewRayH.w - u_cameraPos.xyz);
    float3 worldPos = u_cameraPos.xyz + rayDir * cameraDistance;
    float3 V = normalize(u_cameraPos.xyz - worldPos);

    float3 N      = normalize(normSamp.xyz * 2.0 - 1.0);
    float  NdotV  = saturate(dot(N, V));

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 reflectionFresnel = CompositeFresnelSchlick(NdotV, F0);
    float smoothness = 1.0 - roughness;
    float dielectricFloor = 0.20 * smoothness * smoothness;
    reflectionFresnel = lerp(
        max(reflectionFresnel, float3(dielectricFloor, dielectricFloor, dielectricFloor)),
        reflectionFresnel,
        saturate(metallic));

    // Shared visibility for this explicit reflection-composite pass. Non-reflective
    // or very rough surfaces should not receive traced reflection.
    float roughnessVisibility = 1.0 - smoothstep(0.65, 1.0, roughness);
    float reflectionVisibility = materialReflectionMask * roughnessVisibility;

    // SWRT radiance is scene-hit radiance only. Skybox/IBL is not used as a
    // reflection fallback; misses carry alpha=0 and add nothing.
    // Specular AO is intentionally NOT applied here — SWRT shadow rays already
    // give physically correct visibility at the hit. AO is for IBL (applied in
    // CookTorranceGGX_PS.hlsl). Applying it here would double-count occlusion.
    float3 swrtSpecular = reflSamp.rgb * reflectionFresnel;

    // Blend SWRT confidence into the reflection alpha, suppressing SWRT on
    // non-reflective or very rough surfaces.
    float swrtAlpha = saturate(reflSamp.a * u_reflectionParams.y) * reflectionVisibility;

    // The PSO adds this RGB contribution to the current SceneColor.
    float3 output = swrtSpecular * swrtAlpha;

    return float4(output, 0.0);
}
