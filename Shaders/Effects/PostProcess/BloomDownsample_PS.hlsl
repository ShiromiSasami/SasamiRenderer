// BloomDownsample_PS.hlsl -- one step down the bloom mip chain (BloomRenderer).
//
// 13-tap filter from Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare" (SIGGRAPH 2014, slides 153-163): four overlapping 2x2 box groups around the centre
// (weight 0.5) plus four corner groups (weight 0.125 each). The bilinear taps sit on texel
// corners, so 13 fetches cover a 36-texel footprint and the result does not shimmer when
// the camera moves, unlike a plain 2x2 box.
//
// The first step (u_bloomParams.z = 1) uses the paper's "partial Karis average": each 2x2
// group is weighted by 1 / (1 + luma) before it is summed, which stops a single sub-pixel
// highlight from becoming a flickering firefly in the blurred result. The weighting is not
// energy-preserving, so it is done only once, at the full-resolution input.
//
// No threshold, as in the paper: every pixel contributes, and the tone map pass blends the
// result in with a small weight (u_toneMapParams.w), so only very bright pixels show a halo.
// The same formulation is described in learnopengl.com "Physically Based Bloom" (2022).

Texture2D SourceTex : register(t0);
SamplerState LinearWrap : register(s0);

cbuffer BloomCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_bloomParams;         // xy: source texel size, z: 1 = Karis average (first step)
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// The root signature's only linear sampler wraps, so the UV is clamped half a texel inside the
// image instead; otherwise the left edge would bleed light from the right one.
float3 Fetch(float2 uv)
{
    const float2 halfTexel = u_bloomParams.xy * 0.5;
    return max(SourceTex.SampleLevel(LinearWrap, clamp(uv, halfTexel, 1.0 - halfTexel), 0).rgb, 0.0);
}

float KarisWeight(float3 c)
{
    const float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    return 1.0 / (1.0 + luma);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float2 t = u_bloomParams.xy;
    const float2 uv = input.uv;

    // a - b - c
    // - j - k -
    // d - e - f
    // - l - m -
    // g - h - i
    const float3 a = Fetch(uv + t * float2(-2.0, -2.0));
    const float3 b = Fetch(uv + t * float2( 0.0, -2.0));
    const float3 c = Fetch(uv + t * float2( 2.0, -2.0));
    const float3 d = Fetch(uv + t * float2(-2.0,  0.0));
    const float3 e = Fetch(uv);
    const float3 f = Fetch(uv + t * float2( 2.0,  0.0));
    const float3 g = Fetch(uv + t * float2(-2.0,  2.0));
    const float3 h = Fetch(uv + t * float2( 0.0,  2.0));
    const float3 i = Fetch(uv + t * float2( 2.0,  2.0));
    const float3 j = Fetch(uv + t * float2(-1.0, -1.0));
    const float3 k = Fetch(uv + t * float2( 1.0, -1.0));
    const float3 l = Fetch(uv + t * float2(-1.0,  1.0));
    const float3 m = Fetch(uv + t * float2( 1.0,  1.0));

    // Each group is the mean of four taps; the five groups then weigh 0.5 + 4 * 0.125 = 1.
    const float3 groupCentre = (j + k + l + m) * 0.25;
    const float3 groupTopLeft = (a + b + d + e) * 0.25;
    const float3 groupTopRight = (b + c + e + f) * 0.25;
    const float3 groupBottomLeft = (d + e + g + h) * 0.25;
    const float3 groupBottomRight = (e + f + h + i) * 0.25;

    float3 result;
    if (u_bloomParams.z > 0.5) {
        const float wC = KarisWeight(groupCentre) * 0.5;
        const float wTL = KarisWeight(groupTopLeft) * 0.125;
        const float wTR = KarisWeight(groupTopRight) * 0.125;
        const float wBL = KarisWeight(groupBottomLeft) * 0.125;
        const float wBR = KarisWeight(groupBottomRight) * 0.125;
        result = (groupCentre * wC + groupTopLeft * wTL + groupTopRight * wTR +
                  groupBottomLeft * wBL + groupBottomRight * wBR) /
                 max(wC + wTL + wTR + wBL + wBR, 1e-4);
    } else {
        result = groupCentre * 0.5 +
                 (groupTopLeft + groupTopRight + groupBottomLeft + groupBottomRight) * 0.125;
    }
    return float4(result, 1.0);
}
