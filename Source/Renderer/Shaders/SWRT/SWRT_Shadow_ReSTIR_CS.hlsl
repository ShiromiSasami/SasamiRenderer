//
// SWRT_Shadow_ReSTIR_CS.hlsl
// Standalone shadow pass – ReSTIR-based point/spot light shadow for the raster pipeline.
//
// For each pixel: reads the GBuffer (written by SWRT_ReSTIR_Initial_CS), picks the
// M=4 best candidate lights via Weighted Reservoir Sampling, casts one shadow ray to
// the selected light, and outputs the effective shadow radiance in R16G16B16A16_FLOAT.
//
// Bindings:
//   b0  : ReSTIRFrameConstants
//   t0-t5 : BVH SRVs (for shadow rays)
//   t6  : g_gbuffer   (GBuffer normal/depth – scratch SRV[0])
//   t12 : g_pointLights (inline SRV)
//   t13 : g_spotLights  (inline SRV)
//   u0  : g_shadowOut   (R16G16B16A16_FLOAT – scratch UAV[0])
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

Texture2D<float4> g_gbuffer : register(t6);  // scratch SRV[0]

struct GpuPointLightRT { float3 pos; float range; float3 colorIntensity; float pad; };
struct GpuSpotLightRT  { float3 pos; float range; float3 dir; float cosInner;
                         float3 colorIntensity; float cosOuter; };
StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

RWTexture2D<float4> g_shadowOut : register(u0);  // scratch UAV[0]

// ---- Candidate count ----
static const uint kM = 4u;

// ---- Reconstruct world position from GBuffer ----
float3 ReconstructWorldPos(uint2 pixel, float linearDepth)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(g_renderWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(g_renderHeight)) * 2.0f + 1.0f;
    float4 viewRayH = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    float3 viewRay  = normalize(viewRayH.xyz / viewRayH.w - g_cameraPos);
    return g_cameraPos + viewRay * linearDepth;
}

// ---- Evaluate p_hat for a candidate light ----
float EvalPhatShadow(uint i, float3 pos, float3 N)
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

[numthreads(16, 16, 1)]
void CS_Shadow_ReSTIR(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    float4 gbuf  = g_gbuffer[id.xy];
    float  depth = gbuf.w;

    if (depth < 0.0f)
    {
        g_shadowOut[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 N        = normalize(gbuf.xyz);
    float3 worldPos = ReconstructWorldPos(id.xy, depth);

    uint totalLights = g_pointLightCount + g_spotLightCount;
    if (totalLights == 0u)
    {
        g_shadowOut[id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    // ---- WRS: sample M=4 candidate lights ----
    Reservoir r = InitReservoir();
    uint rngState = ReSTIRSeed(id.xy, g_frameIndex, 7u);

    uint stride = max(1u, totalLights / kM);
    for (uint k = 0u; k < kM; ++k)
    {
        uint li     = (k * stride + (uint)(Rand01(rngState) * float(stride))) % totalLights;
        float phat  = EvalPhatShadow(li, worldPos, N);
        float pdf   = 1.0f / float(totalLights);
        float w     = (pdf > 1e-10f) ? phat / pdf : 0.0f;
        UpdateReservoir(r, li, w, Rand01(rngState));
    }
    FinalizeReservoir(r, EvalPhatShadow(r.lightIndex, worldPos, N));

    // ---- Shadow ray for selected light ----
    float3 lightContrib = float3(0.0f, 0.0f, 0.0f);

    if (r.lightIndex != 0xFFFFFFFFu && r.W > 0.0f)
    {
        uint   li       = r.lightIndex;
        float3 lightPos = float3(0, 0, 0);
        float3 lightRad = float3(0, 0, 0);
        float  lightRange = 0.0f;
        bool   valid    = true;

        if (li < g_pointLightCount)
        {
            GpuPointLightRT pl = g_pointLights[li];
            lightPos   = pl.pos;
            lightRad   = pl.colorIntensity;
            lightRange = pl.range;
        }
        else
        {
            uint si = li - g_pointLightCount;
            GpuSpotLightRT sl = g_spotLights[si];
            lightPos   = sl.pos;
            lightRad   = sl.colorIntensity;
            lightRange = sl.range;
            float3 toL = normalize(lightPos - worldPos);
            float  cosA = dot(-toL, normalize(sl.dir));
            if (cosA >= sl.cosOuter)
                lightRad *= smoothstep(sl.cosOuter, sl.cosInner, cosA);
            else
                valid = false;
        }

        if (valid)
        {
            float3 toLight = lightPos - worldPos;
            float  dist    = length(toLight);
            if (dist < lightRange)
            {
                float3 L     = toLight / dist;
                float  NdotL = max(dot(N, L), 0.0f);
                bool   inShadow = TraceAnyHit(OffsetRay(worldPos, N), L, 0.0f, dist - 0.001f);
                if (!inShadow && NdotL > 0.0f)
                {
                    float t     = dist / max(lightRange, 1e-4f);
                    float atten = saturate(1.0f - t * t) * (1.0f - t * t);
                    lightContrib = lightRad * atten * NdotL * r.W;
                }
            }
        }
    }

    g_shadowOut[id.xy] = float4(lightContrib, 1.0f);
}
