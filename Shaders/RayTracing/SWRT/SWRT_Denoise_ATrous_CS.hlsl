//
// SWRT_Denoise_ATrous_CS.hlsl
// Pass 6 of the ReSTIR DI pipeline – à-trous edge-preserving wavelet filter.
//
// Run 5 times with step widths 1, 2, 4, 8, 16 (set g_stepWidth in the CBuf).
// Uses a standard 5×5 kernel (B-spline weights) with edge-stopping functions
// based on colour luminance, world normal, and depth.
//
// Bindings:
//   b0  : ReSTIRFrameConstants  (g_stepWidth used here)
//   t6  : g_colorIn  (input colour  – scratch SRV[0])
//   t7  : g_gbuffer  (GBuffer normal/depth – scratch SRV[1])
//   u0  : g_colorOut (output colour – scratch UAV[0])
//

cbuffer ReSTIRFrameConstants : register(b0)
{
    row_major float4x4 g_invVP;
    row_major float4x4 g_prevVP;
    float3 g_cameraPos;
    float  g_tMin;
    uint   g_renderWidth;
    uint   g_renderHeight;
    uint   g_frameIndex;
    uint   g_reservoirWidth;
    float  g_temporalAlpha;
    float  g_phiColor;   // ~4.0
    float  g_phiNormal;  // ~128.0
    float  g_phiDepth;   // ~1.0
    float  g_stepWidth;  // 1, 2, 4, 8, 16
    float  g_maxSurfaceRoughness;
    float  g_maxPrimaryHitDistance;
    float  g_minReflectionEnergy;
    float3 g_dirLightDir;
    float  g_dirLightIntensity;
    float3 g_dirLightColor;
    float  g_shadowBias;
    float3 g_ambientColor;
    float  g_ambientIntensity;
    uint   g_pointLightCount;
    uint   g_spotLightCount;
    uint   g_cbPad0;
    uint   g_cbPad1;
};

Texture2D<float4>   g_colorIn  : register(t6);   // scratch SRV[0]
Texture2D<float4>   g_gbuffer  : register(t7);   // scratch SRV[1]
RWTexture2D<float4> g_colorOut : register(u0);   // scratch UAV[0]

// 5×5 à-trous kernel weights (B-spline approximation)
static const float kKernel[3] = { 0.375f, 0.25f, 0.0625f };
// Offsets: 0, ±1*step, ±2*step

float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

[numthreads(16, 16, 1)]
void CS_Denoise_ATrous(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    float4 centerColor = g_colorIn[id.xy];
    float4 centerGbuf  = g_gbuffer[id.xy];
    float  centerDepth = centerGbuf.w;

    // Sky / miss: pass through
    if (centerDepth < 0.0f)
    {
        g_colorOut[id.xy] = centerColor;
        return;
    }

    float3 centerN    = normalize(centerGbuf.xyz);
    float  centerLum  = Luminance(centerColor.rgb);
    int    step        = int(g_stepWidth + 0.5f);

    // ---------------------------------------------------------------------------
    // Variance-guided filter bandwidth (SVGF: Schied et al. SIGGRAPH 2017).
    // Estimate local luminance variance from a 3×3 neighbourhood and scale
    // phi_color proportionally: high variance → wider filter (more denoising),
    // low variance → narrow filter (preserve detail / edges).
    // ---------------------------------------------------------------------------
    float lumSum = 0.0f, lumSqSum = 0.0f, varCount = 0.0f;
    for (int vy = -1; vy <= 1; ++vy)
    {
        for (int vx = -1; vx <= 1; ++vx)
        {
            int2 vPix = clamp(int2(id.xy) + int2(vx, vy),
                              int2(0, 0), int2(int(g_renderWidth) - 1, int(g_renderHeight) - 1));
            float4 vGbuf = g_gbuffer[uint2(vPix)];
            if (vGbuf.w < 0.0f) continue;  // skip sky taps
            float vLum   = Luminance(g_colorIn[uint2(vPix)].rgb);
            lumSum   += vLum;
            lumSqSum += vLum * vLum;
            varCount += 1.0f;
        }
    }
    float meanLum  = lumSum / max(varCount, 1.0f);
    float variance = max(0.0f, lumSqSum / max(varCount, 1.0f) - meanLum * meanLum);
    // Adaptive phi_color: g_phiColor is a scale factor; multiply by sqrt(variance)
    // so the edge-stop widens in noisy regions and tightens in converged ones.
    float phiAdaptive = max(g_phiColor * sqrt(variance + 1e-10f), 1e-4f);

    float3 colorAccum  = float3(0, 0, 0);
    float  weightAccum = 0.0f;

    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 tapPixel = int2(id.xy) + int2(dx, dy) * step;
            if (tapPixel.x < 0 || tapPixel.x >= int(g_renderWidth) ||
                tapPixel.y < 0 || tapPixel.y >= int(g_renderHeight))
                continue;

            float4 tapColor = g_colorIn[uint2(tapPixel)];
            float4 tapGbuf  = g_gbuffer[uint2(tapPixel)];
            if (tapGbuf.w < 0.0f) continue;

            // Kernel weight
            float kw = kKernel[abs(dx)] * kKernel[abs(dy)];

            // Edge-stopping: colour (variance-guided bandwidth)
            float lumDiff = Luminance(tapColor.rgb) - centerLum;
            float wColor  = exp(-lumDiff * lumDiff / (phiAdaptive * phiAdaptive + 1e-10f));

            // Edge-stopping: normal
            float3 tapN   = normalize(tapGbuf.xyz);
            float  nDot   = max(dot(centerN, tapN), 0.0f);
            float  wNormal = pow(nDot, g_phiNormal);

            // Edge-stopping: depth
            float depthDiff = abs(tapGbuf.w - centerDepth);
            float wDepth    = exp(-depthDiff * depthDiff / (g_phiDepth * g_phiDepth + 1e-10f));

            float w = kw * wColor * wNormal * wDepth;
            colorAccum  += tapColor.rgb * w;
            weightAccum += w;
        }
    }

    float3 result = (weightAccum > 1e-6f) ? colorAccum / weightAccum : centerColor.rgb;
    g_colorOut[id.xy] = float4(result, centerColor.a);
}
