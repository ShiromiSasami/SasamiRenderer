// VolumetricCloud_PS.hlsl
//
// Path-tracing style volumetric cloud via raymarching.
//
// Pipeline:
//   1. Reconstruct world-space ray from screen UV + inverse VP.
//   2. Intersect with a horizontal cloud slab (baseAlt .. topAlt).
//   3. March 64 steps through the slab sampling Worley+FBM density.
//   4. For each non-empty step fire 8 shadow-march samples toward the sun
//      to accumulate Beer-Lambert self-shadow (path-tracing style single
//      scattering with multiple-scattering energy powder approximation).
//   5. Apply Henyey-Greenstein anisotropic phase function.
//   6. Output RGBA; alpha = 1 - transmittance 竊・alpha-blended over bg.
//
// CB layout (matches PushCameraCB(mvp=invVP, world, extra0, extra1, extra2)):
//   u_invVP     : inverse view-projection (row-major, row-vector convention)
//   u_worldData : packed params (see field comments below)
//   u_renderSize: xy = viewport size, zw = reserved

cbuffer VolumetricCloudCB : register(b0)
{
    row_major float4x4 u_invVP;      // invVP for ray reconstruction
    row_major float4x4 u_worldData;  // row0: camPos.xyz, time
                                     // row1: sunDir.xyz (toward sun), sunIntensity
                                     // row2: sunColor.rgb, cloudCover [0..1]
                                     // row3: cloudDensity, windSpeed, cloudBaseAlt(m), cloudTopAlt(m)
    float4 u_renderSize;             // x=width, y=height, z=1/width, w=1/height
    float4 u_extra1;                 // reserved
    float4 u_extra2;                 // reserved
};

// ============================================================
// Noise / FBM
// ============================================================

// Interleaved-gradient hash 窶・fast, low-pattern-repetition.
#include "VolumetricCloud_Media.hlsli"


struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

struct PSOutput
{
    float4 color : SV_TARGET0;  // SceneColor (alpha-blended cloud)
};

PSOutput PSMain(PSInput input)
{
    // Unpack CB params.
    float3 camPos    = u_worldData[0].xyz;
    float  sceneTime = u_worldData[0].w;
    float3 sunDir    = normalize(u_worldData[1].xyz);
    float  sunInt    = u_worldData[1].w;
    float3 sunColor  = u_worldData[2].rgb;
    float  cloudCover  = u_worldData[2].w;
    float  density   = u_worldData[3].x;
    float  windSpeed = u_worldData[3].y;
    float  baseAlt   = u_worldData[3].z;
    float  topAlt    = u_worldData[3].w;

    // Reconstruct world-space ray direction from screen UV.
    float2 ndcXY = float2(input.uv.x * 2.0f - 1.0f,
                          1.0f - input.uv.y * 2.0f);
    float4 nearH = mul(float4(ndcXY, 0.0f, 1.0f), u_invVP);
    float4 farH  = mul(float4(ndcXY, 1.0f, 1.0f), u_invVP);
    if (abs(nearH.w) < 1e-7f || abs(farH.w) < 1e-7f)
    {
        PSOutput o; o.color = float4(0,0,0,0); return o;
    }
    float3 nearPt = nearH.xyz / nearH.w;
    float3 farPt  = farH.xyz  / farH.w;
    float3 rayDir = normalize(farPt - nearPt);

    // Don't render clouds below the horizon (downward rays skip the cloud slab).
    if (rayDir.y < -0.02f && camPos.y < baseAlt)
    {
        PSOutput o; o.color = float4(0,0,0,0); return o;
    }

    // Slab intersection.
    float2 tRange = SlabIntersect(camPos, rayDir, baseAlt, topAlt);
    if (tRange.x < 0.0f)
    {
        PSOutput o; o.color = float4(0,0,0,0); return o;
    }

    // Clamp march range to a reasonable maximum (100 km) to avoid artefacts.
    tRange.y = min(tRange.y, tRange.x + 100000.0f);

    // Primary march parameters.
    const int   kSteps    = 64;
    float       tSpan     = tRange.y - tRange.x;
    float       stepSize  = tSpan / float(kSteps);

    // Phase function setup.
    float cosTheta = dot(rayDir, sunDir);
    float phase    = lerp(PhaseHG(cosTheta, 0.6f),   // forward scatter
                          PhaseHG(cosTheta, -0.2f),  // back scatter
                          0.35f);

    // Accumulation.
    float3 scatterAccum = float3(0,0,0);
    float  transmit     = 1.0f;

    for (int s = 0; s < kSteps; ++s)
    {
        float  t   = tRange.x + (float(s) + 0.5f) * stepSize;
        float3 pos = camPos + rayDir * t;

        float  d   = SampleDensity(pos, sceneTime, cloudCover, density,
                                    windSpeed, baseAlt, topAlt);
        if (d <= 0.0f) continue;

        // Extinction coefficient (density ﾃ・absorption coefficient).
        float  sigma = d * 0.015f;            // tuned for km-scale march
        float  sampleT = exp(-sigma * stepSize);
        float  sampleTau = 1.0f - sampleT;

        // Sun lighting: shadow march + HG phase + powder effect.
        float  sunT   = SunTransmittance(pos, sunDir, sceneTime, cloudCover,
                                          density, windSpeed, baseAlt, topAlt);
        // Powder darkening approximates multiple-scattering self-occlusion at
        // high density (dark edges, bright interior).
        float  powder = 1.0f - exp(-sigma * stepSize * 2.0f);
        float  Li     = sunT * sunInt * phase * 2.0f * powder;

        // Accumulate: 竏ｫ ﾏダs ﾂｷ L_i ﾂｷ T dt (energy-conserving in-scattering).
        scatterAccum += transmit * sampleTau * sunColor * Li;

        // Update transmittance.
        transmit *= sampleT;
        if (transmit < 0.005f) break;  // early termination
    }

    // Ambient sky light contribution (uniform bluish fill).
    float3 ambientSky = float3(0.3f, 0.5f, 0.75f) * 0.25f;
    scatterAccum += ambientSky * (1.0f - transmit);

    float alpha = 1.0f - transmit;

    PSOutput o;
    o.color = float4(scatterAccum, alpha);
    return o;
}
