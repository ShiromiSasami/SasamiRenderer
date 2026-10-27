// LightShaft_PS.hlsl -- god rays (crepuscular rays) as a screen-space radial blur.
//
// Kenny Mitchell, "Volumetric Light Scattering as a Post-Process", GPU Gems 3, chapter 13
// (NVIDIA, 2007). The analytic scattering integral along a view ray reduces, for a single
// bright source, to a sum of samples marched toward the light's screen-space position with
// an exponential falloff -- so the whole effect is one radial blur of an occlusion image.
//
// The occlusion image here is the tone-mapped back buffer itself rather than a separate
// mask render: bright unoccluded sky already reads near-white and geometry in front of the
// sun reads far darker, which is the contrast the march needs. That is the cheap variant
// Mitchell describes when a dedicated occlusion pass is not worth its bandwidth; it costs
// some light bleeding off bright surfaces, which is why u_shaftParams.z scales the result
// down rather than adding it at full strength.
//
// The result is blended additively onto the back buffer (the PSO sets ONE/ONE), so this
// shader outputs only the scattered contribution, never the original color.

Texture2D InputTex : register(t0);
SamplerState LinearWrap : register(s0);

cbuffer LightShaftCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_shaftParams;         // xy: sun position in UV space, z: intensity, w: 0 when the sun is behind the camera
};

// GPU Gems 3 chapter 13, listing 13-1. Density scales the sample spacing, decay the
// per-step falloff, weight the per-sample contribution and exposure the final scale.
static const int   kSampleCount = 64;
static const float kDensity     = 0.926f;
static const float kDecay       = 0.96815f;
static const float kWeight      = 0.58767f;
static const float kExposure    = 0.2f;

// Stands in for Mitchell's dedicated occlusion render. Without a mask, every bright pixel
// scatters and a daylit scene washes out completely (measured: 98% of the frame changed at
// intensity 1.2, the whole image blown to white). Keeping only what is brighter than this
// reproduces what the mask would have contained -- the sun and the sky immediately around
// it -- while lit geometry, which sits well below saturation after tone mapping, stops
// contributing. Subtracting rather than clipping keeps the ray edges soft.
static const float kOcclusionThreshold = 0.75f;

float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    // w is the "sun is in front of the camera" flag. Behind it, the screen-space position
    // the projection produces is a mirrored ghost, and marching toward it draws rays that
    // fan out of the wrong edge of the screen.
    if (u_shaftParams.w < 0.5f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float2 sunUv = u_shaftParams.xy;

    // Step from the current pixel toward the sun. Dividing by the sample count spreads the
    // march over the whole distance, so pixels far from the sun take longer strides and the
    // rays stay continuous instead of breaking into dots near the edges.
    float2 delta = (uv - sunUv) * (kDensity / float(kSampleCount));

    float2 sampleUv = uv;
    float3 accumulated = 0.0f.xxx;
    float illuminationDecay = 1.0f;

    [loop]
    for (int i = 0; i < kSampleCount; ++i) {
        sampleUv -= delta;

        // Outside the screen there is nothing sampled; clamping would smear the border
        // pixel along the whole remaining march.
        if (any(sampleUv < 0.0f.xx) || any(sampleUv > 1.0f.xx)) {
            break;
        }

        const float3 sampled = InputTex.SampleLevel(LinearWrap, sampleUv, 0).rgb;

        // Threshold on luminance, then carry the sample's own colour. Subtracting the threshold
        // per channel instead shifts the hue hard toward whichever channel is brightest: a warm
        // source like sunlit stone, say (1.0, 0.6, 0.3), loses green and blue entirely and the
        // shafts come out pure red. Mitchell's original marches an occlusion mask, which is
        // colourless, so the problem does not arise there.
        const float luminance = dot(sampled, float3(0.2126f, 0.7152f, 0.0722f));
        const float emission = saturate((luminance - kOcclusionThreshold) / (1.0f - kOcclusionThreshold));
        accumulated += sampled * emission * illuminationDecay * kWeight;
        illuminationDecay *= kDecay;
    }

    return float4(accumulated * kExposure * u_shaftParams.z, 1.0f);
}
