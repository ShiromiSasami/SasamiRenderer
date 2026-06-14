Texture2D<float4> SceneColorCopyTex : register(t0);
Texture2D<float4> TransparentAccumTex : register(t7);
Texture2D<float> TransparentRevealageTex : register(t8);
SamplerState LinearWrap : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET0
{
    float2 uv = saturate(input.uv);
    float3 sceneColor = SceneColorCopyTex.SampleLevel(LinearWrap, uv, 0).rgb;
    float4 accum = TransparentAccumTex.SampleLevel(LinearWrap, uv, 0);
    float revealage = saturate(TransparentRevealageTex.SampleLevel(LinearWrap, uv, 0));

    if (accum.a <= 1e-5) {
        return float4(sceneColor, 1.0);
    }

    float3 transparentColor = max(accum.rgb / max(accum.a, 1e-5), 0.0);
    float transparentAlpha = saturate(1.0 - revealage);
    float3 composited = lerp(sceneColor, transparentColor, transparentAlpha);
    return float4(composited, 1.0);
}
