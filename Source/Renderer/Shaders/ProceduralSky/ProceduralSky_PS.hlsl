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

#include "ProceduralSky/ProceduralSky.hlsli"

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

    // 2. Volumetric cloud ray march
    float4 clouds = float4(0, 0, 0, 0);
    if (rd.y > 0.01f && cloudCover > 0.01f) {
        clouds = MarchClouds(rd, sunDir, sunColor, sunIntensity,
                             time, cloudCover, cloudDensity);
    }

    // 3. Composite clouds over sky. Tone mapping happens in the post process pass.
    float3 color = lerp(sky, clouds.rgb / max(clouds.a, 1e-4f), saturate(clouds.a));

    return float4(max(color, 0.0f), 1.0f);
}
