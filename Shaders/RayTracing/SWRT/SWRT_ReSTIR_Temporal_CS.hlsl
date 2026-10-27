//
// SWRT_ReSTIR_Temporal_CS.hlsl
// Pass 2 of the ReSTIR reflection DI pipeline.
//
// The current reflection domain is the secondary hit point. Until a previous
// secondary-hit position history is available, this pass only revalidates the
// current initial reservoir at the current secondary hit.
//

#include "RayTracing/SWRT/SWRT_Common.hlsli"
#include "RayTracing/SWRT/SWRT_Reservoir.hlsli"

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
    float  g_microShadowStrength;
    float3 g_ambientColor;
    float  g_ambientIntensity;
    uint   g_pointLightCount;
    uint   g_spotLightCount;
    uint   g_cbPad0;
    uint   g_cbPad1;
    uint   g_punctualLightFalloff; // RendererEnums::PunctualLightFalloff (offset 256)
};

Texture2D<float4> g_gbuffer     : register(t6);
Texture2D<float4> g_prevGBuffer : register(t7);
Texture2D<float4> g_hitPosition : register(t9);

StructuredBuffer<Reservoir>   g_reservoirIn  : register(t14);
StructuredBuffer<Reservoir>   g_prevTemporal : register(t15);
RWStructuredBuffer<Reservoir> g_reservoirOut : register(u3);

#include "RayTracing/SWRT/SWRT_LightTypes.hlsli"
StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

#include "RayTracing/SWRT/SWRT_ReSTIR_LightEval.hlsli"

[numthreads(16, 16, 1)]
void CS_ReSTIR_Temporal(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    uint pixIdx = id.y * g_reservoirWidth + id.x;
    Reservoir r = g_reservoirIn[pixIdx];

    float4 gbuf = g_gbuffer[id.xy];
    float4 hitPos = g_hitPosition[id.xy];
    if (gbuf.w < 0.0f || hitPos.w <= 0.0f)
    {
        g_reservoirOut[pixIdx] = r;
        return;
    }

    float3 N = normalize(gbuf.xyz);

    // Temporal reuse (Bitterli et al. 2020, Alg. 4; Wyman et al. 2023).
    // Reproject the current secondary hit with the previous view-projection.
    // The previous secondary hit position is not bound, so the history pixel is
    // validated against prevGBuffer (normal + hit distance) only.
    if (g_frameIndex > 0u)
    {
        float4 clip = mul(float4(hitPos.xyz, 1.0f), g_prevVP);
        if (clip.w > 1e-6f)
        {
            float2 ndc    = clip.xy / clip.w;
            float2 prevUV = ndc * float2(0.5f, -0.5f) + 0.5f;
            if (all(prevUV >= 0.0f) && all(prevUV < 1.0f))
            {
                int2   prevPx    = int2(prevUV * float2(g_renderWidth, g_renderHeight));
                float4 prevGbuf  = g_prevGBuffer[uint2(prevPx)];
                bool   prevValid = prevGbuf.w >= 0.0f;

                // Geometric validation: relative depth (10%) and normal (cos 0.9).
                float depthDiff = abs(prevGbuf.w - gbuf.w) / max(abs(gbuf.w), 1e-4f);
                prevValid = prevValid && (depthDiff <= 0.1f);
                prevValid = prevValid && (dot(normalize(prevGbuf.xyz), N) >= 0.9f);

                if (prevValid)
                {
                    uint      prevIdx = uint(prevPx.y) * g_reservoirWidth + uint(prevPx.x);
                    Reservoir prev    = g_prevTemporal[prevIdx];

                    if (prev.M > 0u && prev.lightIndex != 0xFFFFFFFFu &&
                        isfinite(prev.W) && prev.W > 0.0f)
                    {
                        // M-cap: history confidence limited to 20x current (Bitterli 2020, Sec. 6).
                        prev.M = min(prev.M, 20u * r.M);

                        uint  rngState = ReSTIRSeed(id.xy, g_frameIndex, 1u);
                        float pHatPrev = EvalPhat(prev.lightIndex, hitPos.xyz, N);
                        CombineReservoir(r, prev, pHatPrev, Rand01(rngState));
                    }
                }
            }
        }
    }

    float p_hat_y = EvalPhat(r.lightIndex, hitPos.xyz, N);
    FinalizeReservoir(r, p_hat_y);
    g_reservoirOut[pixIdx] = r;
}
