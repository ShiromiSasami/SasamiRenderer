//
// SWRT_ReSTIR_Initial_CS.hlsl
// Pass 1 of the ReSTIR DI pipeline.
//
// Each thread processes one screen pixel:
//   1. Trace a primary camera ray to obtain the first hit (GBuffer data).
//   2. Sample M=8 candidate lights with Weighted Reservoir Sampling (WRS).
//   3. Store the GBuffer (world normal + NDC depth) and the initial reservoir.
//
// Bindings (see GpuSoftwareRayTracer::CreateReSTIRPipelines for root layout):
//   b0  : ReSTIRFrameConstants (inline CBV)
//   t0-t5 : BVH SRVs
//   t6-t11 : per-pass extra SRVs  (scratch – unused in this pass)
//   u0  : g_gbufferOut   (RWTexture2D<float4>, scratch UAV[0])
//   t12 : g_pointLights  (StructuredBuffer, inline SRV)
//   t13 : g_spotLights   (StructuredBuffer, inline SRV)
//   u3  : g_reservoirOut (RWStructuredBuffer<Reservoir>, inline UAV)
//

#include "SWRT/SWRT_Common.hlsli"
#include "SWRT/SWRT_Reservoir.hlsli"

// --------------------------------------------------------------------------
// Per-dispatch constants (shared across all ReSTIR passes)
// --------------------------------------------------------------------------
cbuffer ReSTIRFrameConstants : register(b0)
{
    row_major float4x4 g_invVP;
    row_major float4x4 g_prevVP;
    float3 g_cameraPos;
    float  g_tMin;
    uint   g_renderWidth;
    uint   g_renderHeight;
    uint   g_frameIndex;
    uint   g_reservoirWidth;
    float  g_temporalAlpha;
    float  g_phiColor;
    float  g_phiNormal;
    float  g_phiDepth;
    float  g_stepWidth;
    float  g_maxSurfaceRoughness;
    float  g_maxPrimaryHitDistance;
    float  g_minReflectionEnergy;
    float3 g_dirLightDir;
    float  g_dirLightIntensity;
    float3 g_dirLightColor;
    float  g_shadowBias;
    float3 g_ambientColor;
    float  g_ambientIntensity;
    uint   g_pointLightCount;
    uint   g_spotLightCount;
    uint   g_cbPad0;
    uint   g_cbPad1;
};

// --------------------------------------------------------------------------
// Light structures (match GpuSoftwareRayTracer::GpuPointLightRT / GpuSpotLightRT)
// --------------------------------------------------------------------------
struct GpuPointLightRT { float3 pos; float range; float3 colorIntensity; float pad; };
struct GpuSpotLightRT  { float3 pos; float range; float3 dir; float cosInner;
                         float3 colorIntensity; float cosOuter; };

StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

// Outputs
RWTexture2D<float4>           g_gbufferOut    : register(u0);  // normal.xyz + NDC depth
RWStructuredBuffer<Reservoir> g_reservoirOut  : register(u3);

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------
float3 ComputeCameraRayDir(uint2 pixel)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(g_renderWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(g_renderHeight)) * 2.0f + 1.0f;
    float4 dir = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    return normalize(dir.xyz / dir.w - g_cameraPos);
}

// --------------------------------------------------------------------------
// Count total lights (point + spot)
// --------------------------------------------------------------------------
uint TotalLightCount() { return g_pointLightCount + g_spotLightCount; }

// Evaluate p_hat for light index i (merged point+spot list)
float EvalPhat(uint i, float3 pos, float3 N)
{
    if (i < g_pointLightCount)
    {
        GpuPointLightRT pl = g_pointLights[i];
        return PhatPoint(pos, N, pl.pos, pl.colorIntensity, pl.range);
    }
    uint si = i - g_pointLightCount;
    GpuSpotLightRT sl = g_spotLights[si];
    // Spot cone pre-cull
    float3 toLight = sl.pos - pos;
    float  dist    = length(toLight);
    if (dist >= sl.range) return 0.0f;
    float3 L = toLight / dist;
    float  cosA = dot(-L, normalize(sl.dir));
    if (cosA < sl.cosOuter) return 0.0f;
    float spotAtten = smoothstep(sl.cosOuter, sl.cosInner, cosA);
    return PhatPoint(pos, N, sl.pos, sl.colorIntensity * spotAtten, sl.range);
}

// --------------------------------------------------------------------------
[numthreads(16, 16, 1)]
void CS_ReSTIR_Initial(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    uint pixIdx = id.y * g_reservoirWidth + id.x;

    // Trace primary camera ray
    float3 rayDir  = ComputeCameraRayDir(id.xy);
    float3 rayOrig = g_cameraPos;

    // Validate TLAS
    if (g_tlasNodes[0].leftChild == 0 && g_tlasNodes[0].rightOrCount == 0)
    {
        g_gbufferOut[id.xy] = float4(0, 0, 0, -1.0f);  // depth=-1 = sky/miss
        g_reservoirOut[pixIdx] = InitReservoir();
        return;
    }

    HitResult primary = TraceClosestHit(rayOrig, rayDir, g_tMin, g_maxPrimaryHitDistance);
    if (!primary.hit)
    {
        g_gbufferOut[id.xy] = float4(0, 0, 0, -1.0f);  // sky
        g_reservoirOut[pixIdx] = InitReservoir();
        return;
    }

    float3 hitPos  = rayOrig + rayDir * primary.t;
    float3 hitNorm = GetWorldNormal(primary);
    if (dot(hitNorm, -rayDir) < 0.0f) hitNorm = -hitNorm;

    // Compute NDC depth of hit point (for reprojection)
    float4 clipPos = mul(float4(hitPos, 1.0f), transpose(g_invVP)); // invVP^-T = VP, approximate
    // Actually we want: clipPos = VP * worldPos. We have invVP, not VP.
    // Workaround: store linear depth (distance from camera) instead.
    float linearDepth = primary.t;  // world-space distance from camera

    // Write GBuffer: normal.xyz + linearDepth
    g_gbufferOut[id.xy] = float4(hitNorm, linearDepth);

    // --- Initial candidate sampling (M=8 WRS) ---
    uint totalLights = TotalLightCount();
    Reservoir r = InitReservoir();

    if (totalLights == 0u)
    {
        r.W = 0.0f;
        g_reservoirOut[pixIdx] = r;
        return;
    }

    static const uint kM = 8u;
    uint rngState = ReSTIRSeed(id.xy, g_frameIndex, 0u);

    for (uint s = 0; s < kM; ++s)
    {
        // Pick random light
        uint lightIdx = (uint)(Rand01(rngState) * float(totalLights));
        lightIdx = min(lightIdx, totalLights - 1u);

        float w = EvalPhat(lightIdx, hitPos, hitNorm) * float(totalLights);
        UpdateReservoir(r, lightIdx, w, Rand01(rngState));
    }

    // Compute final W
    float p_hat_y = (r.lightIndex != 0xFFFFFFFFu)
                  ? EvalPhat(r.lightIndex, hitPos, hitNorm)
                  : 0.0f;
    FinalizeReservoir(r, p_hat_y);

    g_reservoirOut[pixIdx] = r;
}
