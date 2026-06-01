TextureCube SkyboxTex : register(t0);
SamplerState LinearWrap : register(s0);

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

float3 ApplyDirectionalLightMarker(float3 color, float3 sampleDir)
{
    if (u_skyboxMarkerParams.x <= 0.5 || u_directionalLightColor.a <= 0.0) {
        return color;
    }

    const float3 lightDir = normalize(u_directionalLightDir.xyz);
    const float cosTheta = saturate(dot(sampleDir, lightDir));
    const float discMask = smoothstep(cos(u_skyboxMarkerParams.y), 1.0, cosTheta);
    const float haloMask = smoothstep(cos(u_skyboxMarkerParams.z), 1.0, cosTheta);
    const float3 markerColor =
        lerp(saturate(u_directionalLightColor.rgb), float3(1.0, 1.0, 1.0), 0.35);

    return color + markerColor * ((haloMask * 0.5) + (discMask * 6.0)) * u_skyboxMarkerParams.w;
}

float4 PSMain(PSInput i) : SV_TARGET
{
    float3 sampleDir = normalize(i.dir);
    float3 color = SkyboxTex.Sample(LinearWrap, sampleDir).rgb;
    color = ApplyDirectionalLightMarker(color, sampleDir);

    return float4(color, 1.0);
}
