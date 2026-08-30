#ifndef SASAMI_SKYBOX_COMMON_HLSLI
#define SASAMI_SKYBOX_COMMON_HLSLI

// Shared camera/marker constants and directional-light marker shading,
// used by both Skybox_HDR_PS.hlsl and Skybox_PS.hlsl.

cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
    float4 u_directionalLightDir;
    float4 u_directionalLightColor;
    float4 u_skyboxMarkerParams;
}

struct PSInput
{
    float4 position : SV_POSITION;
    float3 dir      : TEXCOORD0;
};

float3 ApplyDirectionalLightMarker(float3 color, float3 sampleDir, float markerLerpFactor,
                                    float haloWeight, float discWeight, bool doSaturate)
{
    if (u_skyboxMarkerParams.x <= 0.5 || u_directionalLightColor.a <= 0.0) {
        return color;
    }

    const float3 lightDir = normalize(u_directionalLightDir.xyz);
    const float cosTheta = saturate(dot(sampleDir, lightDir));
    const float discMask = smoothstep(cos(u_skyboxMarkerParams.y), 1.0, cosTheta);
    const float haloMask = smoothstep(cos(u_skyboxMarkerParams.z), 1.0, cosTheta);
    const float3 markerColor =
        lerp(saturate(u_directionalLightColor.rgb), float3(1.0, 1.0, 1.0), markerLerpFactor);

    const float3 result = color + markerColor * ((haloMask * haloWeight) + (discMask * discWeight)) * u_skyboxMarkerParams.w;
    return doSaturate ? saturate(result) : result;
}

#endif
