//
// SWRT_ReSTIR_Temporal_CS.hlsl
// Pass 2 of the ReSTIR reflection DI pipeline.
//
// The current reflection domain is the secondary hit point. Until a previous
// secondary-hit position history is available, this pass only revalidates the
// current initial reservoir at the current secondary hit.
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

Texture2D<float4> g_gbuffer     : register(t6);
Texture2D<float4> g_prevGBuffer : register(t7);
Texture2D<float4> g_hitPosition : register(t9);

StructuredBuffer<Reservoir>   g_reservoirIn  : register(t14);
StructuredBuffer<Reservoir>   g_prevTemporal : register(t15);
RWStructuredBuffer<Reservoir> g_reservoirOut : register(u3);

struct GpuPointLightRT { float3 pos; float range; float3 colorIntensity; float pad; };
struct GpuSpotLightRT  { float3 pos; float range; float3 dir; float cosInner;
                         float3 colorIntensity; float cosOuter; };
StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

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
    float p_hat_y = EvalPhat(r.lightIndex, hitPos.xyz, N);
    FinalizeReservoir(r, p_hat_y);
    g_reservoirOut[pixIdx] = r;
}
