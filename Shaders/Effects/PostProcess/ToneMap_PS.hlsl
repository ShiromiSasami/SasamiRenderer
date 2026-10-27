Texture2D SceneColorTex : register(t0);
// Half-resolution bloom chain top (BloomRenderer). Only read when u_toneMapParams.w > 0; while
// bloom is off the slot holds SceneColor again so the table is never left unbound.
Texture2D BloomTex : register(t1);
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
    float4 u_bloomScale;          // x: 1 / bloom mip count (BloomRenderer::GetResultScale), turns
                                  //    the summed mip chain into a mean; rest of the matrix slot unused
    float4 u_unused0[3];          // camera-CB layout padding (first matrix slot), unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_toneMapParams;       // x: exposure, y: HDR10 output enabled (0/1), z: paper-white nits,
                                  // w: bloom blend weight (0 = off)
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// IEC 61966-2-1 sRGB encode: linear toe below 0.0031308, 1.055 x^(1/2.4) - 0.055 above.
// Replaces a pure 2.2 gamma, which over-darkens the near-black range against the real curve.
float3 LinearToSrgb(float3 color)
{
    const float3 c = saturate(color);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
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

// Rec.709 (scene-linear) -> Rec.2020 (scene-linear) primaries conversion, D65 white point.
// Coefficients from ITU-R BT.2087-0 "Colour conversion from Recommendation ITU-R BT.709 to
// Recommendation ITU-R BT.2020" Table 4. The same matrix is used by Microsoft's
// DirectX-Graphics-Samples D3D12HDR sample and by Unreal Engine's Rec709ToRec2020 conversion, so
// it is the de-facto standard one for this exact conversion in game engines.
float3 Rec709ToRec2020(float3 color)
{
    static const float3x3 kRec709ToRec2020 =
    {
        0.627404, 0.329283, 0.043313,
        0.069097, 0.919540, 0.011362,
        0.016391, 0.088013, 0.895595
    };
    return mul(kRec709ToRec2020, color);
}

// SMPTE ST 2084 (PQ, "Perceptual Quantizer") constants, exactly as defined by the standard and
// reproduced in ITU-R BT.2100-2 Table 4 and Microsoft's D3D12HDR sample's PQ encode helper.
static const float PQ_m1 = 2610.0 / 16384.0;
static const float PQ_m2 = 2523.0 / 4096.0 * 128.0;
static const float PQ_c1 = 3424.0 / 4096.0;
static const float PQ_c2 = 2413.0 / 4096.0 * 32.0;
static const float PQ_c3 = 2392.0 / 4096.0 * 32.0;

// SMPTE ST 2084 OETF: encodes scene-linear light -- normalized so that 1.0 == 10000 nits -- into
// PQ code values in [0, 1] (10-bit values for DXGI_FORMAT_R10G10B10A2_UNORM).
float3 LinearToPq(float3 linearColor)
{
    float3 Lm1 = pow(max(linearColor, 0.0), PQ_m1);
    float3 numerator = PQ_c1 + PQ_c2 * Lm1;
    float3 denominator = 1.0 + PQ_c3 * Lm1;
    return pow(numerator / denominator, PQ_m2);
}

// Assumed display peak luminance for the HDR10 highlight shoulder. Should eventually come from
// the display's DXGI_OUTPUT_DESC1::MaxLuminance.
static const float HDR10_MAX_NITS = 1000.0;

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 hdrColor = max(SceneColorTex.SampleLevel(LinearWrap, saturate(input.uv), 0).rgb, 0.0);
    // Bloom is mixed in scene-linear space before exposure, as a lerp rather than an add, so the
    // total energy stays the same and the weight reads as "fraction of light that scatters"
    // (learnopengl.com "Physically Based Bloom", 2022; Jimenez 2014 adds instead).
    if (u_toneMapParams.w > 0.0)
    {
        const float3 bloom = max(BloomTex.SampleLevel(LinearWrap, saturate(input.uv), 0).rgb, 0.0) * u_bloomScale.x;
        hdrColor = lerp(hdrColor, bloom, u_toneMapParams.w);
    }
    float3 exposedColor = hdrColor * u_toneMapParams.x;

    // HDR10 output path (RenderSettings::enableHdrOutput, DX12 raster path only -- see
    // Dx12GraphicsDeviceInit.cpp and Renderer::ResizeViewport for how the swap chain gets there).
    // u_toneMapParams.y is set to exactly 0 or 1 by RendererGraphicsCommands.cpp, so this
    // threshold is only ever comparing against one of those two values.
    if (u_toneMapParams.y > 0.5)
    {
        // Deliberately skip AcesFilmic + LinearToSrgb here instead of reusing them: AcesFilmic is
        // a fit of the ACES RRT+ODT tuned so that inputs around 1.0 map close to a 100-nit SDR
        // display's peak white, and LinearToSrgb is the SDR gamma-2.2-ish OETF. Applying either to
        // HDR values compresses everything meant to reach the display's real HDR peak (hundreds to
        // thousands of nits) right back down toward that SDR peak -- discarding exactly the extra
        // dynamic range HDR10 output exists to preserve. This is why HDR output paths generally
        // replace the SDR tone curve rather than reuse it verbatim. This renderer has no
        // HDR-targeted ACES ODT variant implemented (a real one is a much larger, separate effort),
        // so this path instead passes the exposed scene-linear colour straight into the
        // colour-space conversion and PQ encode, and relies on the encode's own compressive shape
        // for highlight roll-off; the final saturate() below only guards the 10-bit output range.
        float3 rec2020Color = Rec709ToRec2020(exposedColor);

        // Paper-white scaling: colour value 1.0 (this renderer's SDR reference white) becomes
        // u_toneMapParams.z nits, then normalized against the PQ curve's 10000-nit reference.
        const float kPqReferenceWhiteNits = 10000.0;
        float3 nits = max(rec2020Color * u_toneMapParams.z, 0.0);

        // Highlight shoulder: below the knee the signal is untouched; above it the curve
        // approaches HDR10_MAX_NITS asymptotically instead of hard-clipping at the display peak.
        // Same idea as the BT.2390 EETF knee/roll-off (ITU-R BT.2390-10 section 5.4.1), using a
        // simple exponential shoulder that is C1-continuous at the knee. Applied per channel.
        const float kneeNits = 0.75 * HDR10_MAX_NITS;
        const float shoulderRange = HDR10_MAX_NITS - kneeNits;
        float3 shoulder = kneeNits + shoulderRange * (1.0 - exp(-(nits - kneeNits) / shoulderRange));
        nits = select(nits <= kneeNits, nits, shoulder);
        float3 pqInput = max(nits / kPqReferenceWhiteNits, 0.0);

        return float4(saturate(LinearToPq(pqInput)), 1.0);
    }

    float3 mapped = AcesFilmic(exposedColor);
    return float4(LinearToSrgb(mapped), 1.0);
}
