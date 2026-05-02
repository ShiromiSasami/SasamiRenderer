cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
    float4 u_materialBaseColor;
    float4 u_materialEmissiveRoughness;
    float4 u_materialParams;
}

Texture2D AlbedoTex : register(t0);
SamplerState LinearWrap : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

float4 PSMain(PSInput i) : SV_TARGET
{
    float4 texColor = AlbedoTex.Sample(LinearWrap, i.uv);
    float3 baseColor = texColor.rgb * i.color.rgb * u_materialBaseColor.rgb;
    return float4(baseColor + u_materialEmissiveRoughness.rgb, 1.0f);
}
