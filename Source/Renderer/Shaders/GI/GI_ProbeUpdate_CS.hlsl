//
// GI_ProbeUpdate_CS.hlsl
// Incremental irradiance probe update using SWRT BVH ray tracing.
//
// Dispatch:  (probesThisDispatch, 1, 1) thread groups
// Threads:   (64, 1, 1) per group — one group per probe, 64 rays per probe
//
// Each thread traces one ray from the probe centre into the scene,
// projects the returned radiance onto L2 SH (9 coefficients), and
// contributes to a groupshared parallel reduction.
// Thread 0 then blends the new SH with the existing probe data via EMA and writes back.
//

#include "SWRT/SWRT_Common.hlsli"

// --------------------------------------------------------------------------
// Per-dispatch constants  (b0)
// Must match GIUpdateCBData in IrradianceProbeGrid.h
// --------------------------------------------------------------------------
cbuffer GIUpdateCB : register(b0)
{
    // Probe grid layout
    float3 g_probeOrigin;
    float  g_pad0;
    float3 g_probeSpacing;
    float  g_pad1;
    uint   g_probeCountX;
    uint   g_probeCountY;
    uint   g_probeCountZ;
    uint   g_baseProbeIndex;      // First global probe index in this dispatch batch

    // Temporal blending
    float  g_emaAlpha;            // EMA blend weight for new samples (e.g. 0.1)
    float  g_maxTraceDistance;    // Maximum ray distance (scene extent)
    float  g_shadowBias;
    uint   g_frameIndex;          // Monotonic counter for jitter decoration

    // Directional light
    float3 g_dirLightDir;         // Direction *towards* the light (normalised)
    float  g_dirLightIntensity;
    float3 g_dirLightColor;
    float  g_ambientIntensity;

    // Ambient / sky colour
    float3 g_ambientColor;
    uint   g_probesThisDispatch;  // Number of probes updated in this call
}

// --------------------------------------------------------------------------
// Probe SH output buffer
// Layout: float4[9] per probe, indexed as [probeIndex * 9 + coeffIndex].
// --------------------------------------------------------------------------
RWStructuredBuffer<float4> g_probeSHOutput : register(u0);

// --------------------------------------------------------------------------
// Groupshared reduction buffer
// 9 SH coefficients × 64 threads × float3 = 9 × 64 × 12 = 6912 bytes < 32 KB limit
// --------------------------------------------------------------------------
static const uint kRaysPerProbe = 64u;
static const uint kSHCount      = 9u;

groupshared float3 gs_shAccum[kSHCount][kRaysPerProbe];

// --------------------------------------------------------------------------
// SH projection helpers
// --------------------------------------------------------------------------

// Accumulate radiance projected onto L2 SH basis into an array of float3[9].
void ProjectOntoSH(float3 dir, float3 radiance, inout float3 sh[kSHCount])
{
    float x = dir.x, y = dir.y, z = dir.z;
    sh[0] += radiance * 0.282095f;
    sh[1] += radiance * (0.488603f * y);
    sh[2] += radiance * (0.488603f * z);
    sh[3] += radiance * (0.488603f * x);
    sh[4] += radiance * (1.092548f * x * y);
    sh[5] += radiance * (1.092548f * y * z);
    sh[6] += radiance * (0.315392f * (3.0f * z * z - 1.0f));
    sh[7] += radiance * (1.092548f * x * z);
    sh[8] += radiance * (0.546274f * (x * x - y * y));
}

// --------------------------------------------------------------------------
// Direction sampling — Fibonacci lattice on unit sphere
// Produces uniform, low-discrepancy sample directions.
// jitter (0-1) offsets the sequence for temporal decorrelation.
// --------------------------------------------------------------------------
float3 FibonacciSphereDir(uint i, uint n, float2 jitter)
{
    const float kGoldenAngle = 2.39996323f;
    float fi    = float(i) + frac(jitter.x);
    float theta = acos(clamp(1.0f - 2.0f * fi / float(n), -1.0f, 1.0f));
    float phi   = kGoldenAngle * (fi + jitter.y * float(n));
    float sinT  = sin(theta);
    return normalize(float3(sinT * cos(phi), cos(theta), sinT * sin(phi)));
}

// --------------------------------------------------------------------------
// Minimal PBR shade at hit point (NEE with directional light + ambient)
// --------------------------------------------------------------------------

float3 FresnelSchlickGI(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float GGX_D_GI(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265f * d * d, 1e-7f);
}

float GGX_V_GI(float NdotL, float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gL = NdotL / max(NdotL * (1.0f - k) + k, 1e-5f);
    float gV = NdotV / max(NdotV * (1.0f - k) + k, 1e-5f);
    return gL * gV;
}

float3 ShadePBRAtHit(float3 pos, float3 N, float3 V, GpuMaterial mat)
{
    float roughness = saturate(mat.roughness);
    float3 F0 = SWRT_MaterialF0(mat);
    float3 diffuseReflectance = SWRT_MaterialDiffuseReflectance(mat);
    float3 outColor = max(mat.emissive, 0.0f);

    // Directional light NEE
    float3 L = normalize(g_dirLightDir);
    float NdotL = max(dot(N, L), 0.0f);
    if (NdotL > 0.0f)
    {
        bool inShadow = TraceAnyHit(pos + N * g_shadowBias, L, 0.001f, 200.0f);
        if (!inShadow)
        {
            float3 H     = normalize(L + V);
            float NdotV  = max(dot(N, V), 0.001f);
            float NdotH  = saturate(dot(N, H));
            float VdotH  = saturate(dot(V, H));
            float3 F     = FresnelSchlickGI(VdotH, F0);
            float  D     = GGX_D_GI(NdotH, max(roughness, 0.05f));
            float  Vis   = GGX_V_GI(NdotL, NdotV, max(roughness, 0.05f));
            float3 spec  = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
            float3 kd    = (1.0f - F);
            float3 diff  = kd * diffuseReflectance / 3.14159265f;
            outColor    += (diff + spec) * NdotL * g_dirLightColor * g_dirLightIntensity;
        }
    }

    // Sky ambient
    outColor += diffuseReflectance * g_ambientColor * g_ambientIntensity;
    return outColor;
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

[numthreads(64, 1, 1)]
void CS_ProbeUpdate(
    uint3 groupId  : SV_GroupID,
    uint  threadId : SV_GroupThreadID)
{
    uint localIdx  = groupId.x;
    uint globalIdx = g_baseProbeIndex + localIdx;

    uint totalProbes = g_probeCountX * g_probeCountY * g_probeCountZ;
    if (globalIdx >= totalProbes)
        return;

    // Decode 3D grid coordinates
    uint tmp = globalIdx;
    uint3 coord;
    coord.x = tmp % g_probeCountX; tmp /= g_probeCountX;
    coord.y = tmp % g_probeCountY; tmp /= g_probeCountY;
    coord.z = tmp;

    float3 probePos = g_probeOrigin + float3(coord) * g_probeSpacing;

    // ---- Initialise groupshared ----
    [unroll]
    for (uint ci = 0; ci < kSHCount; ++ci)
        gs_shAccum[ci][threadId] = float3(0, 0, 0);
    GroupMemoryBarrierWithGroupSync();

    // ---- Trace ray for this thread ----
    // Temporal + probe-index jitter to decorrelate samples across frames and probes.
    float2 jitter = float2(
        frac(float(g_frameIndex) * 0.61803398875f + float(globalIdx) * 0.31415926f),
        frac(float(g_frameIndex) * 0.38196601125f + float(globalIdx) * 0.27182818f)
    );
    float3 dir = FibonacciSphereDir(threadId, kRaysPerProbe, jitter);

    float3 radiance;
    HitResult hit = TraceClosestHit(probePos, dir, 0.01f, g_maxTraceDistance);

    if (!hit.hit)
    {
        // Sky contribution — scale with vertical component for horizon gradient
        float upness   = saturate(dir.y * 0.5f + 0.5f);
        float skyScale = lerp(0.4f, 1.6f, upness);
        radiance = g_ambientColor * (g_ambientIntensity * skyScale);
    }
    else
    {
        GpuInstanceInfo inst = g_instances[hit.instanceIndex];
        GpuMaterial     mat  = g_materials[inst.materialIndex];
        float3 hitPos   = probePos + dir * hit.t;
        float3 hitNorm  = GetWorldNormal(hit);
        if (dot(hitNorm, -dir) < 0.0f) hitNorm = -hitNorm;
        radiance = ShadePBRAtHit(hitPos, hitNorm, -dir, mat);
    }

    // Project onto SH  (weight = 4π / N for uniform sphere sampling)
    const float kWeight = 4.0f * 3.14159265f / float(kRaysPerProbe);
    float3 contrib[kSHCount];
    [unroll] for (uint c = 0; c < kSHCount; ++c) contrib[c] = float3(0, 0, 0);
    ProjectOntoSH(dir, radiance * kWeight, contrib);

    // Write per-thread contribution into groupshared
    [unroll]
    for (uint ci = 0; ci < kSHCount; ++ci)
        gs_shAccum[ci][threadId] = contrib[ci];
    GroupMemoryBarrierWithGroupSync();

    // ---- Parallel reduction tree (log2(64) = 6 passes) ----
    [unroll]
    for (uint stride = 32u; stride >= 1u; stride >>= 1u)
    {
        if (threadId < stride)
        {
            [unroll]
            for (uint ci = 0; ci < kSHCount; ++ci)
                gs_shAccum[ci][threadId] += gs_shAccum[ci][threadId + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // ---- Thread 0 blends and writes ----
    if (threadId == 0)
    {
        uint base = globalIdx * kSHCount;
        [unroll]
        for (uint ci = 0; ci < kSHCount; ++ci)
        {
            float3 newVal  = gs_shAccum[ci][0];
            float3 oldVal  = g_probeSHOutput[base + ci].rgb;
            float3 blended = lerp(oldVal, newVal, g_emaAlpha);
            g_probeSHOutput[base + ci] = float4(blended, 0.0f);
        }
    }
}
