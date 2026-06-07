//
// GI_ProbeUpdate_CS.hlsl
// Incremental irradiance probe update using SWRT BVH ray tracing.
//
// Dispatch:  (probesThisDispatch, 1, 1) thread groups
// Threads:   (64, 1, 1) per group 窶・one group per probe, 64 rays per probe
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
// 9 SH coefficients ﾃ・64 threads ﾃ・float3 = 9 ﾃ・64 ﾃ・12 = 6912 bytes < 32 KB limit
// --------------------------------------------------------------------------
static const uint kRaysPerProbe = 64u;
static const uint kSHCount      = 9u;

groupshared float3 gs_shAccum[kSHCount][kRaysPerProbe];

// --------------------------------------------------------------------------
// SH projection helpers
// --------------------------------------------------------------------------

// Accumulate radiance projected onto L2 SH basis into an array of float3[9].
#include "GI_ProbeTracing.hlsli"

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
        // Sky contribution 窶・scale with vertical component for horizon gradient
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

    // Project onto SH  (weight = 4ﾏ / N for uniform sphere sampling)
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
