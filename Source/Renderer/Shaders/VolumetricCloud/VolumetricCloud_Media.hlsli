#ifndef SASAMI_VOLUMETRIC_CLOUD_MEDIA_HLSLI
#define SASAMI_VOLUMETRIC_CLOUD_MEDIA_HLSLI

// Cloud noise, density, phase, light transmittance, and slab helpers.

float Hash1(float3 p)
{
    p  = frac(p * float3(443.8975f, 397.2973f, 491.1871f));
    p += dot(p, p.zxy + 19.19f);
    return frac((p.x + p.y) * p.z);
}

// Smooth Perlin-like gradient noise (value noise with cubic hermite).
float SmoothNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0f - 2.0f * f);

    float a = Hash1(i + float3(0,0,0));
    float b = Hash1(i + float3(1,0,0));
    float c = Hash1(i + float3(0,1,0));
    float d = Hash1(i + float3(1,1,0));
    float e = Hash1(i + float3(0,0,1));
    float g = Hash1(i + float3(1,0,1));
    float h = Hash1(i + float3(0,1,1));
    float k = Hash1(i + float3(1,1,1));

    return lerp(lerp(lerp(a,b,u.x), lerp(c,d,u.x), u.y),
                lerp(lerp(e,g,u.x), lerp(h,k,u.x), u.y), u.z);
}

// 5-octave FBM (竊・[0,1], median ~ 0.5).
float FBM(float3 p)
{
    float v = 0.0f, a = 0.5f;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        v += a * SmoothNoise(p);
        p *= 2.01f;
        a *= 0.5f;
    }
    return v;
}

// Worley (cellular) noise: 1 - distance to nearest feature point.
// Returns [0,1]; high value = near a cell center = fluffy cloud core.
float Worley(float3 p)
{
    float3 id  = floor(p);
    float3 fd  = frac(p);
    float  minD = 1.0f;
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        float3 nb  = float3(x, y, z);
        float3 pt  = nb + float3(
            Hash1(id + nb + float3(0, 13, 7)),
            Hash1(id + nb + float3(3, 17, 5)),
            Hash1(id + nb + float3(7,  5, 3)));
        float  d   = length(fd - pt);
        minD = min(minD, d);
    }
    return 1.0f - minD;
}

// ============================================================
// Cloud density
// ============================================================

// Returns volumetric density at world-space point p.
// p.y is altitude in metres (same coordinate system as the scene).
float SampleDensity(float3 p, float time, float cloudCover, float density,
                    float windSpeed, float baseAlt, float topAlt)
{
    // Reject outside slab early.
    float h = p.y;
    if (h < baseAlt || h > topAlt) return 0.0f;

    // Vertical profile: ramp up from base, ramp down to top.
    float slabH    = topAlt - baseAlt;
    float relH     = saturate((h - baseAlt) / slabH);
    float vProfile = saturate(relH * 4.0f) * saturate((1.0f - relH) * 2.0f);

    // Wind animation (east + slight north drift).
    float3 wind   = float3(windSpeed, 0.0f, windSpeed * 0.3f);
    float3 worldP = p * 0.00010f + wind * time;  // 0.0001 竊・km-scale noise

    // Base shape: low-freq FBM.
    float base    = FBM(worldP * 0.8f);

    // Detail: Worley noise for cauliflower billows.
    float detail  = Worley(worldP * 2.5f) * 0.35f;

    // Signed density: positive = inside cloud, negative = outside.
    float raw     = base + detail - (1.0f - cloudCover);
    return saturate(raw * density) * vProfile;
}

// ============================================================
// Phase function & lighting
// ============================================================

// Henyey-Greenstein anisotropic phase function.
float PhaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f);
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(denom, 1.5f));
}

// Beer-Lambert shadow march toward sun: returns transmittance [0,1].
float SunTransmittance(float3 pos, float3 sunDir, float time, float cloudCover,
                       float density, float windSpeed, float baseAlt, float topAlt)
{
    const int   kShadowSteps = 8;
    const float kShadowRange = 1500.0f; // shadow march distance (m)
    const float kStep        = kShadowRange / float(kShadowSteps);

    float tau = 0.0f;
    [unroll] for (int i = 0; i < kShadowSteps; ++i)
    {
        float3 sp = pos + sunDir * (float(i) + 0.5f) * kStep;
        tau += SampleDensity(sp, time, cloudCover, density,
                             windSpeed, baseAlt, topAlt) * kStep * 0.0004f;
    }
    return exp(-tau);
}

// ============================================================
// Ray / slab helpers
// ============================================================

// Intersect infinite horizontal slab [yMin, yMax] with ray (orig, dir).
// Returns (tEntry, tExit) or (-1,-1) on miss.
float2 SlabIntersect(float3 orig, float3 dir, float yMin, float yMax)
{
    float invDy = (abs(dir.y) > 1e-6f) ? (1.0f / dir.y) : 1e30f;
    float t0 = (yMin - orig.y) * invDy;
    float t1 = (yMax - orig.y) * invDy;
    if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
    // Camera inside slab: clamp tEntry to 0.
    t0 = max(t0, 0.0f);
    if (t0 >= t1 || t1 < 0.0f) return float2(-1.0f, -1.0f);
    return float2(t0, t1);
}

// ============================================================
// Main raymarching

#endif // SASAMI_VOLUMETRIC_CLOUD_MEDIA_HLSLI
