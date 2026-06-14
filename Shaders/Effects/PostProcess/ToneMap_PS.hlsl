Texture2D SceneColorTex : register(t0);
SamplerState LinearWrap : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 LinearToSrgb(float3 color)
{
    return pow(saturate(color), 1.0 / 2.2);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 hdrColor = max(SceneColorTex.SampleLevel(LinearWrap, saturate(input.uv), 0).rgb, 0.0);
    float3 mapped = hdrColor / (hdrColor + 1.0);
    return float4(LinearToSrgb(mapped), 1.0);
}
