#ifndef PROCEDURAL_SKY_HLSLI
#define PROCEDURAL_SKY_HLSLI

// Analytic atmospheric sky — shared by ProceduralSky_PS.hlsl and SWRT miss shading.
// MarchClouds is intentionally excluded (48-step loop, too costly for CS miss paths).

#ifndef PI
static const float PI = 3.14159265358979f;
#endif

float Hash3(float3 p)
{
    p = frac(p * float3(443.8975f, 441.423f, 437.195f));
    p += dot(p, p.yxz + 19.19f);
    return frac((p.x + p.y) * p.z);
}

float SmoothNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    return lerp(
        lerp(lerp(Hash3(i),                  Hash3(i + float3(1,0,0)), f.x),
             lerp(Hash3(i + float3(0,1,0)),  Hash3(i + float3(1,1,0)), f.x), f.y),
        lerp(lerp(Hash3(i + float3(0,0,1)),  Hash3(i + float3(1,0,1)), f.x),
             lerp(Hash3(i + float3(0,1,1)),  Hash3(i + float3(1,1,1)), f.x), f.y), f.z);
}

float FBM5(float3 p)
{
    float v = 0.0f;
    float a = 0.5f;
    [unroll]
    for (int i = 0; i < 5; i++) {
        v += a * SmoothNoise(p);
        p = p * 2.02f + float3(73.1f, 61.4f, 53.7f);
        a *= 0.5f;
    }
    return v;
}

float3 ComputeSkyColor(float3 rd, float3 sunDir, float3 sunColor, float sunIntensity)
{
    float cosTheta   = dot(rd, sunDir);
    float elevation  = rd.y;

    float3 zenithColor  = float3(0.05f, 0.15f, 0.65f) * 2.0f;
    float3 horizonColor = float3(0.50f, 0.68f, 0.88f) * 1.5f;
    float3 groundColor  = float3(0.08f, 0.06f, 0.04f);

    float3 skyBase;
    if (elevation >= 0.0f) {
        skyBase = lerp(horizonColor, zenithColor, pow(saturate(elevation), 0.4f));
    } else {
        skyBase = lerp(horizonColor, groundColor, saturate(-elevation * 5.0f));
    }

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

#endif // PROCEDURAL_SKY_HLSLI
