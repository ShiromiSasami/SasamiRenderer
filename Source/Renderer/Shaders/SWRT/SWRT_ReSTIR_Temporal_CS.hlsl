//
// SWRT_ReSTIR_Temporal_CS.hlsl
// Pass 2 of the ReSTIR DI pipeline – temporal reservoir reuse.
//
// Reprojects the current pixel to the previous frame and combines the
// current initial reservoir with the previous frame's temporal reservoir.
//
// Bindings:
//   b0  : ReSTIRFrameConstants (inline CBV)
//   t0-t5 : BVH SRVs (not accessed – bound for root-sig compatibility)
//   t6  : g_gbuffer      (current frame GBuffer – scratch SRV[0])
//   t7  : g_prevGBuffer  (prev frame GBuffer   – scratch SRV[1])
//   t14 : g_reservoirIn  (current-frame initial reservoirs from Pass1, inline SRV)
//   t15 : g_prevTemporal (prev-frame temporal output – ping-pong, inline SRV)
//   u3  : g_reservoirOut (temporal output this frame,              inline UAV)
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
Texture2D<float4> g_prevGBuffer    : register(t7);  // scratch SRV[1]
Texture2D<float4> g_rasterMaterial : register(t8);  // scratch SRV[2]: rasterized GBufferMaterial (r=roughness)

StructuredBuffer<Reservoir>   g_reservoirIn  : register(t14);  // current-frame initial (Pass1 output)
StructuredBuffer<Reservoir>   g_prevTemporal : register(t15);  // prev-frame temporal output (ping-pong)
RWStructuredBuffer<Reservoir> g_reservoirOut : register(u3);

// --------------------------------------------------------------------------
// Light structures (identical to Initial pass – needed for p_hat)
// --------------------------------------------------------------------------
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

// Reconstruct world-space position from GBuffer entry.
// GBuffer: float4(normalW.xyz, linearDepth)
float3 ReconstructWorldPos(uint2 pixel, float linearDepth)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(g_renderWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(g_renderHeight)) * 2.0f + 1.0f;
    float4 viewRayH = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    float3 viewRay  = normalize(viewRayH.xyz / viewRayH.w - g_cameraPos);
    return g_cameraPos + viewRay * linearDepth;
}

// Find the previous-frame pixel for the given world position using g_prevVP.
// Returns false if outside previous frame's view or behind the camera.
bool ReprojectToPrev(float3 worldPos, out int2 prevPixel)
{
    float4 prevClipPos = mul(float4(worldPos, 1.0f), g_prevVP);
    if (prevClipPos.w <= 0.0f) { prevPixel = int2(-1, -1); return false; }
    float2 prevNDC = prevClipPos.xy / prevClipPos.w;
    float2 prevUV  = prevNDC * float2(0.5f, -0.5f) + 0.5f;
    prevPixel = int2(prevUV * float2(g_renderWidth, g_renderHeight));
    return (prevPixel.x >= 0 && prevPixel.x < int(g_renderWidth) &&
            prevPixel.y >= 0 && prevPixel.y < int(g_renderHeight));
}

[numthreads(16, 16, 1)]
void CS_ReSTIR_Temporal(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    uint pixIdx = id.y * g_reservoirWidth + id.x;

    float4 gbuf = g_gbuffer[id.xy];
    float  depth = gbuf.w;

    // Start with the initial reservoir
    Reservoir r = g_reservoirIn[pixIdx];

    // Skip sky pixels
    if (depth < 0.0f)
    {
        g_reservoirOut[pixIdx] = r;
        return;
    }

    float3 worldPos = ReconstructWorldPos(id.xy, depth);
    float3 N        = normalize(gbuf.xyz);

    // Read surface roughness from the rasterized material GBuffer.
    // roughness drives adaptive temporal thresholds:
    //   smooth (low roughness) → tight rejection (specular lobe is view-sensitive)
    //   rough  (high roughness) → loose rejection (diffuse is view-insensitive)
    float roughness = g_rasterMaterial[id.xy].r;

    // Roughness-adaptive normal threshold:
    //   mirror (0.0) → 0.95 (reject if normals differ > ~18°)
    //   diffuse(1.0) → 0.75 (accept up to ~41° difference)
    float normalThresh = lerp(0.95f, 0.75f, roughness);

    // Roughness-adaptive M cap:
    //   mirror → 4× (little history; specular lobe changes with camera)
    //   diffuse → 20× (more history stable)
    uint maxMFactor = max(4u, uint(lerp(4.0f, 20.0f, roughness)));

    // Save initial reservoir before any temporal combination.
    // Used as fallback if the combined reservoir's winner turns out invalid at
    // the current position (p_hat = 0 → W = 0 → black pixels during camera movement).
    Reservoir initialR = r;

    // ---------- Temporal reprojection ----------
    // Project current world position into previous frame's clip space using g_prevVP.
    // No same-pixel fallback: if the world point was outside the previous frustum,
    // there is no valid temporal history and we skip reuse entirely.
    if (g_frameIndex > 0u)
    {
        int2 prevPixel;
        bool reprojected = ReprojectToPrev(worldPos, prevPixel);

        if (reprojected &&
            prevPixel.x >= 0 && prevPixel.x < int(g_renderWidth) &&
            prevPixel.y >= 0 && prevPixel.y < int(g_renderHeight))
        {
            float4 prevGbuf  = g_prevGBuffer[prevPixel];
            float  prevDepth = prevGbuf.w;

            // Ratio-based depth check: same world point seen from rotated camera
            // should have identical linear depth (pure yaw keeps distance constant).
            float depthRatio = prevDepth / max(depth, 1e-4f);
            bool depthOk  = (prevDepth > 0.0f) &&
                            (depthRatio > 0.5f) &&
                            (depthRatio < 2.0f);
            bool normalOk = dot(normalize(prevGbuf.xyz), N) > normalThresh;

            if (depthOk && normalOk)
            {
                uint prevIdx   = uint(prevPixel.y) * g_reservoirWidth + uint(prevPixel.x);
                Reservoir prev = g_prevTemporal[prevIdx];  // prev-frame temporal, not current-frame initial

                // Clamp M: roughness-adaptive cap (mirror=4×, diffuse=20×)
                prev.M = min(prev.M, maxMFactor * r.M + 1u);

                // Merge: p_hat evaluated at current pixel for prev reservoir's light
                float p_hat_prev = EvalPhat(prev.lightIndex, worldPos, N);

                uint rngState = ReSTIRSeed(id.xy, g_frameIndex, 1u);
                CombineReservoir(r, prev, p_hat_prev, Rand01(rngState));
            }
        }
    }

    // Recompute p_hat for the current winner.
    // If the combined reservoir's selected light is invalid at this position
    // (e.g. behind the surface normal after a mismatched reprojection),
    // fall back to the initial reservoir to avoid W=0 → black output.
    float p_hat_y = EvalPhat(r.lightIndex, worldPos, N);
    if (p_hat_y <= 0.0f && initialR.lightIndex != 0xFFFFFFFFu)
    {
        r       = initialR;
        p_hat_y = EvalPhat(r.lightIndex, worldPos, N);
    }
    FinalizeReservoir(r, p_hat_y);

    g_reservoirOut[pixIdx] = r;
}
