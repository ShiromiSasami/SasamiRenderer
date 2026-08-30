Texture2D SceneColorTex : register(t0);
SamplerState LinearWrap : register(s0);

// Runtime exposure multiplier applied before tone mapping, driven by the UI
// "Exposure" slider (RenderSettings::exposure, default 1.3). The renderer has
// no auto-exposure yet, so this is the single knob for overall image brightness.
// Default of 1.3 was chosen after split-sum specular IBL landed: 2.5 was tuned
// while the renderer had no environment specular at all, and the added energy
// pushed sunlit pavement into white clipping. Re-tune whenever a new light path
// is added, or leave it to the user via the slider.
cbuffer ToneMapCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_toneMapParams;       // x: exposure
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 LinearToSrgb(float3 color)
{
    return pow(saturate(color), 1.0 / 2.2);
}

// Narkowicz 2015, "ACES Filmic Tone Mapping Curve" -- a cheap fit of the ACES
// RRT+ODT. Keeps highlights from clipping while letting shadows stay dark, unlike
// the plain Reinhard curve this replaces (which lifted every midtone).
float3 AcesFilmic(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 hdrColor = max(SceneColorTex.SampleLevel(LinearWrap, saturate(input.uv), 0).rgb, 0.0);
    float3 mapped = AcesFilmic(hdrColor * u_toneMapParams.x);
    return float4(LinearToSrgb(mapped), 1.0);
}
