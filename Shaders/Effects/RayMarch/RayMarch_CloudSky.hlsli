#ifndef SASAMI_RAYMARCH_CLOUD_SKY_HLSLI
#define SASAMI_RAYMARCH_CLOUD_SKY_HLSLI

// Procedural cloud volume and sky evaluation helpers.

float3 GradHash3(float3 p)
{
    // Map each integer cell to a pseudo-random unit gradient vector.
    p = float3(dot(p, float3(127.1f, 311.7f,  74.7f)),
               dot(p, float3(269.5f, 183.3f, 246.1f)),
               dot(p, float3(113.5f, 271.9f, 124.6f)));
    float3 g = frac(sin(p) * 43758.5453f) * 2.0f - 1.0f;
    return g / (length(g) + 1e-5f); // safe normalize
}

// 3-D gradient noise in [-1, 1].
// Uses quintic smoothstep (6t^5-15t^4+10t^3) which is C2 continuous,
// eliminating the second-derivative discontinuity that makes Hermite (3t^2-2t^3)
// show subtle grid lines even in value noise.
float GradNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); // quintic

    return lerp(
        lerp(lerp(dot(GradHash3(i),                 f),
                  dot(GradHash3(i + float3(1,0,0)), f - float3(1,0,0)), u.x),
             lerp(dot(GradHash3(i + float3(0,1,0)), f - float3(0,1,0)),
                  dot(GradHash3(i + float3(1,1,0)), f - float3(1,1,0)), u.x), u.y),
        lerp(lerp(dot(GradHash3(i + float3(0,0,1)), f - float3(0,0,1)),
                  dot(GradHash3(i + float3(1,0,1)), f - float3(1,0,1)), u.x),
             lerp(dot(GradHash3(i + float3(0,1,1)), f - float3(0,1,1)),
                  dot(GradHash3(i + float3(1,1,1)), f - float3(1,1,1)), u.x), u.y), u.z);
}

float FBM5(float3 p)
{
    float v = 0.0f, a = 0.5f;
    [unroll]
    for (int i = 0; i < 5; i++) {
        v += a * (GradNoise(p) * 0.5f + 0.5f); // remap [-1,1] -> [0,1]
        p = p * 2.02f + float3(73.1f, 61.4f, 53.7f);
        a *= 0.5f;
    }
    return v;
}

float SampleCloudDensity(float3 worldPos, float time, float cloudCover, float cloudDensity)
{
    // Scale worldPos so XYZ variation is comparable in noise space.
    // Y (altitude 1500-5000m) previously only mapped to 0.15-0.5 (range 0.35),
    // while XZ varied ~4x more depending on view angle 竊・horizontal sheets / banding.
    // heightScale=4 expands Y to 0.6-2.0 (range 1.4) 窶・proper 3D volumetric variation.
    const float heightScale = 4.0f;
    float3 p = float3(worldPos.x, worldPos.y * heightScale, worldPos.z) * 0.0001f;
    p.x += time * 0.008f;
    p.z += time * 0.003f;
    float base   = FBM5(p * 0.8f);
    float detail = FBM5(p * 2.5f + float3(1.4f, 2.1f, 0.9f) + time * 0.002f) * 0.35f;
    float raw = base + detail - (1.0f - cloudCover);
    return max(raw * cloudDensity, 0.0f);
}

float4 MarchClouds(float3 rd, float3 sunDir, float3 sunColor, float sunIntensity,
                   float time, float cloudCover, float cloudDensity, float jitter)
{
    const float kCloudBaseY = 1500.0f;
    const float kCloudTopY  = 5000.0f;
    if (rd.y < 0.005f) return float4(0, 0, 0, 0);
    float tBase = (kCloudBaseY - u_camPos.y) / rd.y;
    float tTop  = (kCloudTopY  - u_camPos.y) / rd.y;
    if (tTop <= 0.0f) return float4(0, 0, 0, 0);
    tBase = max(tBase, 0.0f);
    if (tBase > 300000.0f) return float4(0, 0, 0, 0);

    const int kSteps = 48;
    float dt = (tTop - tBase) / float(kSteps);

    float g = 0.6f, g2 = g * g;
    float cosSun = dot(rd, sunDir);
    float denom  = max(1.0f + g2 - 2.0f * g * cosSun, 1e-6f);
    float phaseHG = (1.0f - g2) / (4.0f * PI * pow(denom, 1.5f));

    float transmittance = 1.0f;
    float3 scattered = 0.0f;

    for (int i = 0; i < kSteps; i++) {
        if (transmittance < 0.02f) break;
        // Per-pixel jitter offsets each ray's start position within the first step,
        // breaking the fixed-step "shell" pattern that causes visible horizontal bands.
        float  t   = tBase + (float(i) + jitter) * dt;
        // Use true world-space positions for the cloud slab. The cloud noise is
        // evaluated at a very low frequency (1e-4 scale), so full world coords
        // remain stable while avoiding the 1024m floating-origin wrap used by water.
        float3 pos = u_camPos + rd * t;
        float density = SampleCloudDensity(pos, time, cloudCover, cloudDensity);
        if (density < 1e-4f) continue;
        float sigmaE = density * 0.0008f;
        float sampleT = exp(-sigmaE * dt);
        float sunAtten = exp(-density * 1.2f);
        float3 sunLit  = sunColor * sunIntensity * sunAtten * phaseHG;
        float3 ambient = float3(0.35f, 0.50f, 0.72f) * 0.28f;
        float3 inScatter = (sunLit + ambient) * density;
        scattered += inScatter * transmittance * (1.0f - sampleT) / max(sigmaE, 1e-6f);
        transmittance *= sampleT;
    }
    return float4(scattered, 1.0f - transmittance);
}

float3 ComputeSkyColor(float3 rd, float3 sunDir, float3 sunColor, float sunIntensity)
{
    float cosTheta  = dot(rd, sunDir);
    float elevation = rd.y;

    float3 zenithColor  = float3(0.05f, 0.15f, 0.65f) * 2.0f;
    float3 horizonColor = float3(0.50f, 0.68f, 0.88f) * 1.5f;
    float3 groundColor  = float3(0.08f, 0.06f, 0.04f);

    float3 skyBase;
    if (elevation >= 0.0f)
        skyBase = lerp(horizonColor, zenithColor, pow(saturate(elevation), 0.4f));
    else
        skyBase = lerp(horizonColor, groundColor, saturate(-elevation * 5.0f));

    float sunInfluence = saturate(cosTheta * 0.5f + 0.5f);
    float3 warmTint = lerp(float3(1,1,1), sunColor * float3(1.1f, 0.9f, 0.7f), sunInfluence * 0.4f);
    skyBase *= warmTint;

    float mie = pow(max(cosTheta, 0.0f), 6.0f) * 0.3f;
    float3 mieColor = sunColor * mie * sunIntensity;

    float horizonFactor = pow(saturate(1.0f - abs(elevation)), 3.0f);
    float3 horizGlow = sunColor
        * lerp(float3(1.0f, 0.7f, 0.4f), float3(1.0f, 0.9f, 0.7f), sunIntensity)
        * horizonFactor * max(cosTheta, 0.0f) * 0.4f * sunIntensity;

    float sunDisc = smoothstep(0.9996f, 1.0f, cosTheta);
    float3 disc = sunColor * sunDisc * 12.0f * sunIntensity;

    return max(skyBase + mieColor + horizGlow + disc, 0.0f);
}

// Lightweight sky for water reflections (no cloud marching)
float3 skyColor(float3 rd)
{
    return ComputeSkyColor(rd, normalize(u_sunDir), u_sunColor, u_sunI);
}

#endif // SASAMI_RAYMARCH_CLOUD_SKY_HLSLI
