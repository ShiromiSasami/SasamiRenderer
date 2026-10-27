// DirectX 11 native ambient-occlusion debug blit.
//
// Exists because of TODO.md's lesson from the reverted Vulkan SSAO attempt: "build a path that can
// display AO on its own first". Opted into with SASAMI_DX11_AO_DEBUG=1, it paints the generated
// visibility term over the finished frame so the term itself can be inspected without having to
// infer it from the shaded result. Fully off -- not merely cheap -- when the variable is unset.
Texture2D    AoTex      : register(t0);
SamplerState PointClamp : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float visibility = AoTex.SampleLevel(PointClamp, input.uv, 0).r;
    return float4(visibility, visibility, visibility, 1.0);
}
