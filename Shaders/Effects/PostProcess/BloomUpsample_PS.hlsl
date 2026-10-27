// BloomUpsample_PS.hlsl -- one step back up the bloom mip chain (BloomRenderer).
//
// 3x3 tent filter (1-2-1 / 2-4-2 / 1-2-1, divided by 16) on the smaller mip, written with
// ONE/ONE blending into the next larger one, so every level ends up holding the sum of all
// coarser levels -- Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare" (SIGGRAPH 2014, slides 164-167). The tent radius is in UV units
// (u_bloomParams.w) rather than texels, so the halo keeps its on-screen size at any
// resolution, as in learnopengl.com "Physically Based Bloom" (2022).

Texture2D SourceTex : register(t0);
SamplerState LinearWrap : register(s0);

cbuffer BloomCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_bloomParams;         // xy: source texel size, w: tent radius in UV
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 Fetch(float2 uv)
{
    const float2 halfTexel = u_bloomParams.xy * 0.5;
    return SourceTex.SampleLevel(LinearWrap, clamp(uv, halfTexel, 1.0 - halfTexel), 0).rgb;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float r = u_bloomParams.w;
    const float2 uv = input.uv;

    float3 sum = Fetch(uv) * 4.0;
    sum += (Fetch(uv + float2(0.0, -r)) + Fetch(uv + float2(-r, 0.0)) +
            Fetch(uv + float2(r, 0.0)) + Fetch(uv + float2(0.0, r))) * 2.0;
    sum += Fetch(uv + float2(-r, -r)) + Fetch(uv + float2(r, -r)) +
           Fetch(uv + float2(-r, r)) + Fetch(uv + float2(r, r));
    return float4(sum * (1.0 / 16.0), 1.0);
}
