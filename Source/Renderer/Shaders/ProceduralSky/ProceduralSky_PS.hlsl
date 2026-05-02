// ProceduralSky_PS.hlsl
// SDF-based procedural sky: analytic atmospheric scattering + FBM volumetric cloud ray marching.
// Reuses the skybox cube mesh (POSITION only) and the standard CameraCB at b0.
// Slot mapping:
//   u_directionalLightDir  -> sun direction (towards sun, world space)
//   u_directionalLightColor -> sun color (rgb) + intensity (a)
//   u_skyboxMarkerParams   -> x=time(s), y=cloudCover(0-1), z=cloudDensity, w=unused

cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
    float4 u_directionalLightDir;
    float4 u_directionalLightColor;
    float4 u_skyboxMarkerParams;
}

struct PSInput
{
    float4 position : SV_POSITION;
    float3 dir      : TEXCOORD0;
};

static const float PI = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Value noise + FBM
// ---------------------------------------------------------------------------
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
    f = f * f * (3.0f - 2.0f * f); // cubic smoothstep
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

// ---------------------------------------------------------------------------
// Atmospheric sky (analytic Rayleigh + Mie approximation)
// ---------------------------------------------------------------------------
float3 ComputeSkyColor(float3 rd, float3 sunDir, float3 sunColor, float sunIntensity)
{
    float cosTheta   = dot(rd, sunDir);
    float elevation  = rd.y; // +1 = zenith, -1 = nadir

    // Sky gradient (zenith blue -> horizon haze -> ground)
    float3 zenithColor  = float3(0.05f, 0.15f, 0.65f) * 2.0f;
    float3 horizonColor = float3(0.50f, 0.68f, 0.88f) * 1.5f;
    float3 groundColor  = float3(0.08f, 0.06f, 0.04f);

    float3 skyBase;
    if (elevation >= 0.0f) {
        skyBase = lerp(horizonColor, zenithColor, pow(saturate(elevation), 0.4f));
    } else {
        skyBase = lerp(horizonColor, groundColor, saturate(-elevation * 5.0f));
    }

    // Warm tint near sun direction
    float sunInfluence = saturate(cosTheta * 0.5f + 0.5f);
    float3 warmTint = lerp(float3(1,1,1), sunColor * float3(1.1f, 0.9f, 0.7f), sunInfluence * 0.4f);
    skyBase *= warmTint;

    // Mie forward-scattering (sun halo)
    float mie = pow(max(cosTheta, 0.0f), 6.0f) * 0.3f;
    float3 mieColor = sunColor * mie * sunIntensity;

    // Horizon glow along sun azimuth
    float horizonFactor = pow(saturate(1.0f - abs(elevation)), 3.0f);
    float3 horizGlow = sunColor
        * lerp(float3(1.0f, 0.7f, 0.4f), float3(1.0f, 0.9f, 0.7f), sunIntensity)
        * horizonFactor * max(cosTheta, 0.0f) * 0.4f * sunIntensity;

    // Sun disc
    float sunDisc = smoothstep(0.9996f, 1.0f, cosTheta);
    float3 disc = sunColor * sunDisc * 12.0f * sunIntensity;

    return max(skyBase + mieColor + horizGlow + disc, 0.0f);
}

// ---------------------------------------------------------------------------
// SDF cloud density (FBM-based signed-distance field in cloud layer)
// ---------------------------------------------------------------------------
float SampleCloudDensity(float3 worldPos, float time, float cloudCover, float cloudDensity)
{
    // Scale: 1 world unit = 1 m, so 0.0001 maps 1 km to 0.1 noise units
    float3 p = worldPos * 0.0001f;
    p.x += time * 0.008f;  // eastward wind
    p.z += time * 0.003f;  // northward drift

    float base   = FBM5(p * 0.8f);
    float detail = FBM5(p * 2.5f + float3(1.4f, 2.1f, 0.9f) + time * 0.002f) * 0.35f;

    // Signed density: positive = inside cloud, coverage shifts threshold
    float raw = base + detail - (1.0f - cloudCover);
    return max(raw * cloudDensity, 0.0f);
}

// ---------------------------------------------------------------------------
// Volumetric cloud ray march through flat slab [1500 m, 5000 m] above camera
// ---------------------------------------------------------------------------
float4 MarchClouds(float3 rd, float3 sunDir, float3 sunColor, float sunIntensity,
                   float time, float cloudCover, float cloudDensity)
{
    const float kCloudBaseY = 1500.0f;
    const float kCloudTopY  = 5000.0f;

    if (rd.y < 0.005f) return float4(0, 0, 0, 0); // below horizon

    float tBase = kCloudBaseY / rd.y;
    float tTop  = kCloudTopY  / rd.y;
    if (tBase > 300000.0f) return float4(0, 0, 0, 0); // too far

    const int kSteps = 48;
    float dt = (tTop - tBase) / float(kSteps);

    // Henyey-Greenstein phase for clouds (g ≈ 0.6)
    float g   = 0.6f;
    float g2  = g * g;
    float cosSun = dot(rd, sunDir);
    float denom  = max(1.0f + g2 - 2.0f * g * cosSun, 1e-6f);
    float phaseHG = (1.0f - g2) / (4.0f * PI * pow(denom, 1.5f));

    float transmittance  = 1.0f;
    float3 scatteredLight = 0.0f;

    for (int i = 0; i < kSteps; i++) {
        if (transmittance < 0.02f) break;

        float  t   = tBase + (float(i) + 0.5f) * dt;
        float3 pos = rd * t; // camera at world origin

        float density = SampleCloudDensity(pos, time, cloudCover, cloudDensity);
        if (density < 1e-4f) continue;

        // Extinction / scattering
        float sigmaE = density * 0.0008f;
        float sampleT = exp(-sigmaE * dt);

        // Lighting: beer-lambert sunlight + ambient sky
        float sunAtten  = exp(-density * 1.2f);
        float3 sunLit   = sunColor * sunIntensity * sunAtten * phaseHG;
        float3 ambient  = float3(0.35f, 0.50f, 0.72f) * 0.28f;

        float3 inScatter = (sunLit + ambient) * density;
        scatteredLight  += inScatter * transmittance * (1.0f - sampleT) / max(sigmaE, 1e-6f);
        transmittance   *= sampleT;
    }

    return float4(scatteredLight, 1.0f - transmittance);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
float4 PSMain(PSInput i) : SV_TARGET
{
    float3 rd           = normalize(i.dir);
    float3 sunDir       = normalize(u_directionalLightDir.xyz);
    float3 sunColor     = u_directionalLightColor.rgb;
    float  sunIntensity = max(u_directionalLightColor.a, 0.0f);

    float time        = u_skyboxMarkerParams.x;
    float cloudCover  = saturate(u_skyboxMarkerParams.y);
    float cloudDensity = max(u_skyboxMarkerParams.z, 0.5f);

    // 1. Atmospheric sky
    float3 sky = ComputeSkyColor(rd, sunDir, sunColor, sunIntensity);

    // 2. Reinhard tone-map sky before cloud compositing
    sky = sky / (sky + 1.0f);

    // 3. Volumetric cloud ray march
    float4 clouds = float4(0, 0, 0, 0);
    if (rd.y > 0.01f && cloudCover > 0.01f) {
        clouds = MarchClouds(rd, sunDir, sunColor, sunIntensity,
                             time, cloudCover, cloudDensity);
    }

    // 4. Composite clouds over sky
    float3 color = lerp(sky, clouds.rgb / max(clouds.a, 1e-4f), saturate(clouds.a));

    // 5. Gamma correction
    color = pow(max(color, 0.0f), 1.0f / 2.2f);

    return float4(color, 1.0f);
}
