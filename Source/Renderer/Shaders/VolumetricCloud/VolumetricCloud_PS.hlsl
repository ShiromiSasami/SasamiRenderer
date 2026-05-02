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
//   6. Output RGBA; alpha = 1 - transmittance → alpha-blended over bg.
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

// Interleaved-gradient hash — fast, low-pattern-repetition.
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

// 5-octave FBM (→ [0,1], median ~ 0.5).
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
    float3 worldP = p * 0.00010f + wind * time;  // 0.0001 → km-scale noise

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
// ============================================================

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

        // Extinction coefficient (density × absorption coefficient).
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

        // Accumulate: ∫ σ_s · L_i · T dt (energy-conserving in-scattering).
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
