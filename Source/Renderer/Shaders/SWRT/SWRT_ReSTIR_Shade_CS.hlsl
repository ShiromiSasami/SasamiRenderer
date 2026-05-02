//
// SWRT_ReSTIR_Shade_CS.hlsl
// Pass 4 of the ReSTIR DI pipeline – final shading.
//
// For each pixel: reads the post-spatial reservoir, casts one shadow ray to
// the selected light, and evaluates full PBR shading weighted by reservoir W.
//
// Bindings:
//   b0  : ReSTIRFrameConstants (inline CBV)
//   t0-t5 : BVH SRVs (for shadow rays)
//   t6  : g_gbuffer    (current frame GBuffer – scratch SRV[0])
//   t7  : g_albedo     (scratch SRV[1] – unused, albedo stored in GBuffer α)
//   t12 : g_pointLights (inline SRV)
//   t13 : g_spotLights  (inline SRV)
//   t14 : g_reservoir   (spatial reservoirs from Pass3, inline SRV)
//   u0  : g_shadedOut   (R16G16B16A16_FLOAT, scratch UAV[0])
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

Texture2D<float4> g_gbuffer         : register(t6);  // scratch SRV[0]: ReSTIR GBuffer (normal + linearDepth)
Texture2D<float4> g_rasterMaterial  : register(t7);  // scratch SRV[1]: rasterized GBufferMaterial (r=roughness, g=metallic)
Texture2D<float4> g_rasterAlbedo    : register(t8);  // scratch SRV[2]: rasterized GBufferAlbedo (rgb=baseColor)

struct GpuPointLightRT { float3 pos; float range; float3 colorIntensity; float pad; };
struct GpuSpotLightRT  { float3 pos; float range; float3 dir; float cosInner;
                         float3 colorIntensity; float cosOuter; };
StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

StructuredBuffer<Reservoir> g_reservoir : register(t14);
RWTexture2D<float4>         g_shadedOut : register(u0);   // scratch UAV[0]

// --------------------------------------------------------------------------
// PBR helpers (copied from SWRT_Reflection_CS.hlsl)
// --------------------------------------------------------------------------
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}
float GGX_D(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d);
}
float GGX_V(float NdotL, float NdotV, float roughness)
{
    float r = roughness + 1.0f; float k = (r * r) / 8.0f;
    float gL = NdotL / (NdotL * (1.0f - k) + k);
    float gV = NdotV / (NdotV * (1.0f - k) + k);
    return gL * gV;
}

float3 EvalPBR(float3 N, float3 L, float3 V, float3 albedo, float roughness, float metallic,
               float3 lightRadiance)
{
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 H    = normalize(L + V);
    float  NdotL = max(dot(N, L), 0.0f);
    float  NdotV = max(dot(N, V), 0.001f);
    float  NdotH = saturate(dot(N, H));
    float  VdotH = saturate(dot(V, H));
    float3 F   = FresnelSchlick(VdotH, F0);
    float  D   = GGX_D(NdotH, max(roughness, 0.05f));
    float  Vis = GGX_V(NdotL, NdotV, max(roughness, 0.05f));
    float3 spec = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
    float3 kd   = (1.0f - F) * (1.0f - metallic);
    return (kd * albedo / 3.14159265f + spec) * NdotL * lightRadiance;
}

float3 ReconstructWorldPos(uint2 pixel, float linearDepth)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(g_renderWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(g_renderHeight)) * 2.0f + 1.0f;
    float4 viewRayH = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    float3 viewRay  = normalize(viewRayH.xyz / viewRayH.w - g_cameraPos);
    return g_cameraPos + viewRay * linearDepth;
}

[numthreads(16, 16, 1)]
void CS_ReSTIR_Shade(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    float4 gbuf  = g_gbuffer[id.xy];
    float  depth = gbuf.w;

    if (depth < 0.0f)
    {
        // Sky: use ambient gradient
        g_shadedOut[id.xy] = float4(g_ambientColor * g_ambientIntensity, 0.0f);
        return;
    }

    float3 N       = normalize(gbuf.xyz);
    float3 worldPos = ReconstructWorldPos(id.xy, depth);
    float3 V       = normalize(g_cameraPos - worldPos);

    uint pixIdx = id.y * g_reservoirWidth + id.x;
    Reservoir r  = g_reservoir[pixIdx];

    // Read actual material from rasterized G-Buffer
    float4 matSample    = g_rasterMaterial.Load(int3(id.xy, 0));
    float4 albedoSample = g_rasterAlbedo.Load(int3(id.xy, 0));
    float3 albedo    = albedoSample.rgb;
    float  roughness = matSample.r;
    float  metallic  = matSample.g;

    float3 color = float3(0, 0, 0);

    // --- Selected light (ReSTIR) ---
    if (r.lightIndex != 0xFFFFFFFFu && r.W > 0.0f)
    {
        uint   totalLights = g_pointLightCount + g_spotLightCount;
        uint   li          = r.lightIndex;
        bool   valid       = li < totalLights;
        float3 lightPos    = float3(0,0,0);
        float3 lightRad    = float3(0,0,0);
        float  lightRange  = 0.0f;

        if (valid && li < g_pointLightCount)
        {
            GpuPointLightRT pl = g_pointLights[li];
            lightPos   = pl.pos;
            lightRad   = pl.colorIntensity;
            lightRange = pl.range;
        }
        else if (valid)
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
                float3 L       = toLight / dist;
                float  NdotL   = max(dot(N, L), 0.0f);
                bool   inShadow = TraceAnyHit(OffsetRay(worldPos, N), L, 0.0f, dist - 0.001f);
                if (!inShadow && NdotL > 0.0f)
                {
                    // Distance attenuation
                    float t     = dist / max(lightRange, 1e-4f);
                    float atten = saturate(1.0f - t * t) * (1.0f - t * t);
                    // ReSTIR contribution: PBR * W (the reservoir weight)
                    color += EvalPBR(N, L, V, albedo, roughness, metallic,
                                     lightRad * atten) * r.W;
                }
            }
        }
    }

    // --- Directional light ---
    {
        float3 L     = normalize(g_dirLightDir);
        float  NdotL = max(dot(N, L), 0.0f);
        if (NdotL > 0.0f)
        {
            bool inShadow = TraceAnyHit(OffsetRay(worldPos, N), L, 0.0f, 200.0f);
            if (!inShadow)
                color += EvalPBR(N, L, V, albedo, roughness, metallic,
                                 g_dirLightColor * g_dirLightIntensity);
        }
    }

    // Ambient
    color += albedo * g_ambientColor * g_ambientIntensity;

    // Roughness attenuation only — Fresnel is applied per-channel in PBR_PS.hlsl.
    // (Same convention as SWRT_Reflection_CS.hlsl: alpha = roughnessAtten, not fresnelWeight.)
    float  roughnessFade = 1.0f - smoothstep(g_maxSurfaceRoughness * 0.7f,
                                              g_maxSurfaceRoughness, roughness);
    float  roughnessAtten = roughnessFade * roughnessFade;

    g_shadedOut[id.xy] = float4(color, roughnessAtten);
}
