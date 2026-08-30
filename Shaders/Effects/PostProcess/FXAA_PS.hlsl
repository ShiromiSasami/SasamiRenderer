// FXAA_PS.hlsl -- fast approximate anti-aliasing, applied to the tone-mapped back buffer.
//
// This is the compact single-pass FXAA formulation from Timothy Lottes' original NVIDIA
// whitepaper ("FXAA", NVIDIA, 2009/2011,
// https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf),
// later shipped in full (with an added iterative edge search) as FXAA 3.11 in NVIDIA
// GameWorks' fxaa3_11.h. This shader implements the whitepaper's compact algorithm --
// 4-corner luma sampling, a gradient-estimated blend direction, and a two-tap +
// four-corner blend with a luma-range clamp fallback -- the well-known compact variant
// that has since been reproduced (with these exact constants) across countless engines
// and shader collections as a smaller alternative to the full iterative-search preset.
//
// FXAA operates on non-linear (post-tonemap, sRGB-encoded) color and needs luma; luma is
// computed here from the sampled RGB directly (rather than a spare alpha channel), since
// this render target carries no reserved alpha channel.

Texture2D InputTex : register(t0);
SamplerState LinearWrap : register(s0);

cbuffer FxaaCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_fxaaParams;          // xy: rcpFrame (1/width, 1/height)
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

static const float kFxaaReduceMin = 1.0 / 128.0;
static const float kFxaaReduceMul = 1.0 / 8.0;
static const float kFxaaSpanMax = 8.0;

float3 SampleColor(float2 uv)
{
    return InputTex.SampleLevel(LinearWrap, saturate(uv), 0).rgb;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float2 uv = saturate(input.uv);
    const float2 rcpFrame = u_fxaaParams.xy;

    const float3 rgbNW = SampleColor(uv + float2(-1.0, -1.0) * rcpFrame);
    const float3 rgbNE = SampleColor(uv + float2( 1.0, -1.0) * rcpFrame);
    const float3 rgbSW = SampleColor(uv + float2(-1.0,  1.0) * rcpFrame);
    const float3 rgbSE = SampleColor(uv + float2( 1.0,  1.0) * rcpFrame);
    const float3 rgbM  = SampleColor(uv);

    const float3 lumaWeights = float3(0.299, 0.587, 0.114);
    const float lumaNW = dot(rgbNW, lumaWeights);
    const float lumaNE = dot(rgbNE, lumaWeights);
    const float lumaSW = dot(rgbSW, lumaWeights);
    const float lumaSE = dot(rgbSE, lumaWeights);
    const float lumaM  = dot(rgbM,  lumaWeights);

    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    const float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kFxaaReduceMul), kFxaaReduceMin);
    const float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -kFxaaSpanMax, kFxaaSpanMax) * rcpFrame;

    const float3 rgbA = 0.5 * (
        SampleColor(uv + dir * (1.0 / 3.0 - 0.5)) +
        SampleColor(uv + dir * (2.0 / 3.0 - 0.5)));
    const float3 rgbB = rgbA * 0.5 + 0.25 * (
        SampleColor(uv + dir * -0.5) +
        SampleColor(uv + dir *  0.5));

    const float lumaB = dot(rgbB, lumaWeights);
    const float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(result, 1.0);
}
