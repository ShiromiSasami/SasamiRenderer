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
    float p_hat_y = EvalPhat(r.lightIndex, hitPos.xyz, N);
    FinalizeReservoir(r, p_hat_y);
    g_reservoirOut[pixIdx] = r;
}
