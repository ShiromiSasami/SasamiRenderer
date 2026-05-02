//
// SWRT_Shadow_CS.hlsl
// Directional shadow map via GPU BVH traversal.
// Each thread writes one texel of the shadow depth map.
//

#include "SWRT/SWRT_Common.hlsli"

// --------------------------------------------------------------------------
// Per-dispatch constants
// --------------------------------------------------------------------------
cbuffer ShadowFrameConstants : register(b0)
{
    row_major float4x4 g_invLightVP;   // inverse of light view-projection
    row_major float4x4 g_lightVP;      // light view-projection (for depth output)
    uint  g_width;
    uint  g_height;
    float g_tMin;
    float g_depthBias;
};

RWTexture2D<float> g_output : register(u0);

// --------------------------------------------------------------------------
// Entry point
// --------------------------------------------------------------------------
[numthreads(16, 16, 1)]
void CS_Shadow(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;

    // NDC of this shadow-map texel
    float ndcX =  ((float(id.x) + 0.5f) / float(g_width))  * 2.0f - 1.0f;
    float ndcY = -((float(id.y) + 0.5f) / float(g_height)) * 2.0f + 1.0f;

    // Reconstruct near/far world points from inverse light VP
    float4 nearH = mul(float4(ndcX, ndcY, 0.0f, 1.0f), g_invLightVP);
    float4 farH  = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invLightVP);
    if (abs(nearH.w) < 1e-7f || abs(farH.w) < 1e-7f)
    {
        g_output[id.xy] = 1.0f;
        return;
    }
    float3 nearPt = nearH.xyz / nearH.w;
    float3 farPt  = farH.xyz  / farH.w;

    float3 delta  = farPt - nearPt;
    float  rayLen = length(delta);
    if (rayLen < 1e-6f)
    {
        g_output[id.xy] = 1.0f;
        return;
    }

    float3 rayDir = delta / rayLen;

    // Trace against BVH — single ClosestHit traversal for depth output
    HitResult hit;
    hit.hit = false;
    if (g_tlasNodes[0].leftChild != 0 || g_tlasNodes[0].rightOrCount != 0)
    {
        hit = TraceClosestHit(nearPt, rayDir, g_tMin, rayLen);
    }

    if (!hit.hit)
    {
        g_output[id.xy] = 1.0f;
        return;
    }

    float3 hitWorld = nearPt + rayDir * hit.t;
    float4 hitClip  = mul(float4(hitWorld, 1.0f), g_lightVP);
    float  depth    = hitClip.z / hitClip.w;
    depth += g_depthBias;
    g_output[id.xy] = saturate(depth);
}
