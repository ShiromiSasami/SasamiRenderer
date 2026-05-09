#include "Common/LightCB.hlsli"

Texture2D GBufferAlbedoTex   : register(t0);
Texture2D GBufferMaterialTex : register(t7);
Texture2D ReflectionTex      : register(t8);
Texture2D GBufferNormalTex   : register(t9);
SamplerState LinearWrap      : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    int debugMode = (int)(u_debugParams.x + 0.5);
    if (debugMode != 0 && debugMode != 15)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float2 uv = saturate(input.uv);
    float3 albedo = saturate(GBufferAlbedoTex.SampleLevel(LinearWrap, uv, 0).rgb);
    float4 material = GBufferMaterialTex.SampleLevel(LinearWrap, uv, 0);
    float4 reflectionSample = ReflectionTex.SampleLevel(LinearWrap, uv, 0);

    float roughness = saturate(material.r);
    float metallic = saturate(material.g);
    float reflectionAlpha = saturate(reflectionSample.a * u_reflectionParams.y);

    float smoothness = 1.0 - roughness;
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // This pass runs after SWRT has produced the current-frame radiance. The main
    // lighting pass is already tone-mapped in this renderer, so apply only a compact
    // material response here rather than adding raw radiance directly.
    float dielectricFloor = 0.12 * smoothness * smoothness;
    float3 specularWeight = max(F0, float3(dielectricFloor, dielectricFloor, dielectricFloor));
    float roughnessVisibility = lerp(smoothness * smoothness, 1.0, metallic);
    // The current renderer composites after the main pass has already tone-mapped
    // SceneColor to LDR. Boost the post contribution so the traced reflection remains
    // visible on top of the existing final-lit color.
    float finalLitVisibilityBoost = lerp(10.0, 4.0, metallic);
    float3 reflected = reflectionSample.rgb *
                       specularWeight *
                       reflectionAlpha *
                       roughnessVisibility *
                       finalLitVisibilityBoost;

    return float4(reflected, 0.0);
}
