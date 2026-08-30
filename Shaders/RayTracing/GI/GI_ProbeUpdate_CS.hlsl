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

#include "RayTracing/SWRT/SWRT_Common.hlsli"
#include "RayTracing/SWRT/SWRT_LightTypes.hlsli"
#include "Effects/Sky/ProceduralSky/ProceduralSky.hlsli"

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

    // Punctual lights (point + spot) contributing to probe-ray hit shading.
    uint   g_pointLightCount;
    uint   g_spotLightCount;
    uint   g_giCbPad2;
    uint   g_giCbPad3;
}

// --------------------------------------------------------------------------
// Probe SH output buffer
// Layout: float4[9] per probe, indexed as [probeIndex * 9 + coeffIndex].
// --------------------------------------------------------------------------
RWStructuredBuffer<float4> g_probeSHOutput : register(u0);

// --------------------------------------------------------------------------
// Punctual light buffers (t6/t7) — same layout as SWRT reflection/ReSTIR.
// --------------------------------------------------------------------------
StructuredBuffer<GpuPointLightRT> g_pointLights : register(t6);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t7);

// --------------------------------------------------------------------------
// Groupshared reduction buffer
// Radiance:  9 SH coeffs x 64 threads x float3 = 9*64*12 = 6912 bytes
// Distance:  9 SH coeffs x 64 threads x float  = 9*64*4  = 2304 bytes
// Total 9216 bytes < 32 KB limit
// --------------------------------------------------------------------------
static const uint kRaysPerProbe = 64u;
static const uint kSHCount      = 9u;

groupshared float3 gs_shAccum[kSHCount][kRaysPerProbe];
groupshared float  gs_distAccum[kSHCount][kRaysPerProbe];

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
    {
        gs_shAccum[ci][threadId]   = float3(0, 0, 0);
        gs_distAccum[ci][threadId] = 0.0f;
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- Trace ray for this thread ----
    // Temporal + probe-index jitter to decorrelate samples across frames and probes.
    float2 jitter = float2(
        frac(float(g_frameIndex) * 0.61803398875f + float(globalIdx) * 0.31415926f),
        frac(float(g_frameIndex) * 0.38196601125f + float(globalIdx) * 0.27182818f)
    );
    float3 dir = FibonacciSphereDir(threadId, kRaysPerProbe, jitter);

    float3 radiance = (float3)0;
    bool hasSceneBvh = (g_tlasNodes[0].leftChild != 0 || g_tlasNodes[0].rightOrCount != 0);
    HitResult hit = (HitResult)0;
    if (hasSceneBvh)
    {
        hit = TraceClosestHit(probePos, dir, 0.01f, g_maxTraceDistance);
    }

    if (!hit.hit)
    {
        // Analytical sky (same ProceduralSky.hlsli model SWRT reflection uses for its miss
        // shading) instead of a flat ambient colour, so probes pick up horizon/zenith
        // gradient and sun glow instead of a uniform constant.
        radiance = ComputeSkyColor(dir, normalize(g_dirLightDir), g_dirLightColor, g_dirLightIntensity)
                 * g_ambientIntensity;
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

    // Directional visibility signal: how far this probe can see along `dir`
    // before hitting a surface. Misses read as "open" (g_maxTraceDistance),
    // so the resulting SH-fit distance field naturally rejects blend
    // contributions across occluders (floors/ceilings/walls) at sample time.
    float dist = hit.hit ? hit.t : g_maxTraceDistance;

    // Project onto SH (weight = 4*pi / N for uniform sphere sampling)
    const float kWeight = 4.0f * 3.14159265f / float(kRaysPerProbe);
    float3 contrib[kSHCount];
    float  distContrib[kSHCount];
    [unroll] for (uint c = 0; c < kSHCount; ++c) { contrib[c] = float3(0, 0, 0); distContrib[c] = 0.0f; }
    ProjectOntoSH(dir, radiance * kWeight, contrib);
    ProjectOntoSH(dir, dist * kWeight, distContrib);

    // Write per-thread contribution into groupshared
    [unroll]
    for (uint ci = 0; ci < kSHCount; ++ci)
    {
        gs_shAccum[ci][threadId]   = contrib[ci];
        gs_distAccum[ci][threadId] = distContrib[ci];
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- Parallel reduction tree (log2(64) = 6 passes) ----
    [unroll]
    for (uint stride = 32u; stride >= 1u; stride >>= 1u)
    {
        if (threadId < stride)
        {
            [unroll]
            for (uint ci = 0; ci < kSHCount; ++ci)
            {
                gs_shAccum[ci][threadId]   += gs_shAccum[ci][threadId + stride];
                gs_distAccum[ci][threadId] += gs_distAccum[ci][threadId + stride];
            }
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
            float3 newRad   = gs_shAccum[ci][0];
            float  newDist  = gs_distAccum[ci][0];
            float3 oldRad   = g_probeSHOutput[base + ci].rgb;
            float  oldDist  = g_probeSHOutput[base + ci].w;
            float3 blendedRad  = lerp(oldRad,  newRad,  g_emaAlpha);
            float  blendedDist = lerp(oldDist, newDist, g_emaAlpha);
            g_probeSHOutput[base + ci] = float4(blendedRad, blendedDist);
        }
    }
}
