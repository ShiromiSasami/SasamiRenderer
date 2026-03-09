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
    // Opaque pass only: no lighting evaluation in this shader.
    // If no texture is bound, fallback to white and preserve vertex tint.
    float4 texColor = AlbedoTex.Sample(LinearWrap, i.uv);
    float3 baseColor = (texColor.a > 0.001f) ? texColor.rgb : float3(1.0f, 1.0f, 1.0f);
    return float4(baseColor, 1.0f) * i.color;
}
