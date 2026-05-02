//
// SWRT_ReSTIR_Spatial_CS.hlsl
// Pass 3 of the ReSTIR DI pipeline – spatial reservoir reuse.
//
// Combines the current pixel's reservoir with k=4 random neighbours,
// subject to geometric similarity tests (normal + depth).
//
// Bindings:
//   b0  : ReSTIRFrameConstants (inline CBV)
//   t0-t5 : BVH SRVs (not accessed – bound for root-sig compatibility)
//   t6  : g_gbuffer  (current frame GBuffer – scratch SRV[0])
//   t14 : g_reservoirIn  (temporal reservoirs from Pass2, inline SRV)
//   u2  : g_reservoirOut (spatial output,                 inline UAV)
//

#include "SWRT/SWRT_Common.hlsli"
#include "SWRT/SWRT_Reservoir.hlsli"

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

Texture2D<float4> g_gbuffer        : register(t6);  // scratch SRV[0]
Texture2D<float4> g_rasterMaterial : register(t7);  // scratch SRV[1]: rasterized GBufferMaterial (r=roughness)

struct GpuPointLightRT { float3 pos; float range; float3 colorIntensity; float pad; };
struct GpuSpotLightRT  { float3 pos; float range; float3 dir; float cosInner;
                         float3 colorIntensity; float cosOuter; };
StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

StructuredBuffer<Reservoir>   g_reservoirIn  : register(t14);
RWStructuredBuffer<Reservoir> g_reservoirOut : register(u2);

float EvalPhat(uint i, float3 pos, float3 N)
{
    if (i == 0xFFFFFFFFu) return 0.0f;
    uint totalLights = g_pointLightCount + g_spotLightCount;
    if (i >= totalLights) return 0.0f;
    if (i < g_pointLightCount)
    {
        GpuPointLightRT pl = g_pointLights[i];
        return PhatPoint(pos, N, pl.pos, pl.colorIntensity, pl.range);
    }
    uint si = i - g_pointLightCount;
    GpuSpotLightRT sl = g_spotLights[si];
    float3 toLight = sl.pos - pos;
    float  dist    = length(toLight);
    if (dist >= sl.range) return 0.0f;
    float3 L = toLight / dist;
    float  cosA = dot(-L, normalize(sl.dir));
    if (cosA < sl.cosOuter) return 0.0f;
    float spotA = smoothstep(sl.cosOuter, sl.cosInner, cosA);
    return PhatPoint(pos, N, sl.pos, sl.colorIntensity * spotA, sl.range);
}

float3 ReconstructWorldPos(int2 pixel, float linearDepth)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(g_renderWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(g_renderHeight)) * 2.0f + 1.0f;
    float4 viewRayH = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    float3 viewRay  = normalize(viewRayH.xyz / viewRayH.w - g_cameraPos);
    return g_cameraPos + viewRay * linearDepth;
}

// Maximum spatial search radius (pixels) for fully diffuse surfaces (roughness=1).
// Smooth/specular surfaces use a much smaller radius so the reflection lobe
// neighbourhood stays within the valid specular footprint.
static const int  kRadius     = 30;
static const uint kNeighbours = 4u;

[numthreads(16, 16, 1)]
void CS_ReSTIR_Spatial(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    uint pixIdx = id.y * g_reservoirWidth + id.x;
    float4 gbuf  = g_gbuffer[id.xy];
    float  depth = gbuf.w;

    Reservoir r = g_reservoirIn[pixIdx];

    if (depth < 0.0f)
    {
        g_reservoirOut[pixIdx] = r;
        return;
    }

    float3 worldPos = ReconstructWorldPos(int2(id.xy), depth);
    float3 N        = normalize(gbuf.xyz);

    // Roughness-adaptive search radius.
    // Specular surfaces (roughness ≈ 0) use a tiny radius (≈ 2 px) so spatial
    // reuse only combines near-identical reflection lobes.
    // Fully diffuse surfaces (roughness = 1) use the full kRadius.
    float roughness = g_rasterMaterial[id.xy].r;
    float effectiveRadius = max(2.0f, float(kRadius) * roughness);

    // Roughness-adaptive normal threshold for neighbour acceptance.
    // Smooth surfaces require tighter normal agreement (small lobe → big sensitivity).
    float normalAcceptThresh = lerp(0.95f, 0.80f, roughness);

    uint rngState  = ReSTIRSeed(id.xy, g_frameIndex, 2u);
    uint initialM  = r.M;  // center pixel's sample count before spatial reuse

    // ---------------------------------------------------------------------------
    // Z-weighting buffers (Bitterli et al. 2020, Appendix A).
    // After combining all neighbours we check which of them can plausibly have
    // generated the winning sample, giving an unbiased Z denominator.
    // ---------------------------------------------------------------------------
    float3 nPos[kNeighbours];
    float3 nNor[kNeighbours];
    uint   nM  [kNeighbours];
    uint   validCount = 0u;

    for (uint k = 0; k < kNeighbours; ++k)
    {
        // Random neighbour in disk of roughness-adaptive radius
        float angle  = Rand01(rngState) * 6.28318530718f;
        float radius = Rand01(rngState) * effectiveRadius;
        int2  offset = int2(int(radius * cos(angle) + 0.5f),
                            int(radius * sin(angle) + 0.5f));
        int2  nPixel = int2(id.xy) + offset;

        if (nPixel.x < 0 || nPixel.x >= int(g_renderWidth) ||
            nPixel.y < 0 || nPixel.y >= int(g_renderHeight))
            continue;

        float4 nGbuf  = g_gbuffer[uint2(nPixel)];
        float  nDepth = nGbuf.w;
        if (nDepth < 0.0f) continue;

        // Geometric similarity tests (normal threshold is roughness-adaptive)
        float3 nN = normalize(nGbuf.xyz);
        if (dot(nN, N) < normalAcceptThresh)            continue;  // normal threshold
        if (abs(nDepth - depth) > depth * 0.1f + 0.5f) continue;  // depth threshold

        uint nIdx = uint(nPixel.y) * g_reservoirWidth + uint(nPixel.x);
        Reservoir nb = g_reservoirIn[nIdx];

        float3 nWorldPos = ReconstructWorldPos(nPixel, nDepth);
        float  p_hat_nb  = EvalPhat(nb.lightIndex, worldPos, N);  // at cur pixel's domain

        // Cap neighbour's M to avoid over-confident neighbours dominating
        Reservoir nb_capped = nb;
        nb_capped.M = min(nb.M, initialM * 20u + 1u);

        CombineReservoir(r, nb_capped, p_hat_nb, Rand01(rngState));

        // Record valid neighbour for Z-weighting pass
        nPos[validCount] = nWorldPos;
        nNor[validCount] = nN;
        nM  [validCount] = nb_capped.M;
        validCount++;
    }

    // ---------------------------------------------------------------------------
    // Unbiased finalize via Z-weighting (ReSTIR appendix A).
    // Z = sum of M values over pixels where the winning sample has non-zero p_hat.
    // Dividing wSum by (Z * p_hat_y) gives an unbiased estimate.
    // ---------------------------------------------------------------------------
    float p_hat_y = EvalPhat(r.lightIndex, worldPos, N);

    float Z = (p_hat_y > 0.0f) ? float(initialM) : 0.0f;  // center pixel
    for (uint j = 0; j < validCount; ++j)
    {
        float p_hat_at_nb = EvalPhat(r.lightIndex, nPos[j], nNor[j]);
        if (p_hat_at_nb > 0.0f) Z += float(nM[j]);
    }

    // Z-corrected contribution weight
    r.W = (Z > 0.0f && p_hat_y > 1e-30f) ? (r.wSum / (Z * p_hat_y)) : 0.0f;
    r.M = uint(Z);

    g_reservoirOut[pixIdx] = r;
}
