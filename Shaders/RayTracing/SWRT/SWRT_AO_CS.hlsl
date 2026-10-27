#include "RayTracing/SWRT/SWRT_Common.hlsli"

cbuffer AmbientOcclusionFrameConstants : register(b0)
{
    row_major float4x4 g_invVP;
    float3 g_cameraPos;
    float  g_tMin;
    uint   g_renderWidth;
    uint   g_renderHeight;
    uint   g_sampleCount;
    uint   g_frameIndex;
    float  g_radius;
    float  g_power;
    uint   g_gbufferWidth;
    uint   g_gbufferHeight;
    uint   g_pad0;
    uint   g_pad1;
};

Texture2D<float4> g_gbufferNormal : register(t6);
RWTexture2D<float4> g_output : register(u0);

static const float PI = 3.14159265f;

float2 Hammersley(uint i, uint N)
{
    uint bits = (i << 16u) | (i >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) / float(N), float(bits) * 2.3283064365386963e-10f);
}

float3 CosineSampleHemisphere(float2 Xi)
{
    float r = sqrt(Xi.x);
    float phi = 2.0f * PI * Xi.y;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0f, 1.0f - Xi.x));
    return float3(x, y, z);
}

[numthreads(16, 16, 1)]
void CS_AO(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight)
        return;

    uint2 gbufPx = uint2(
        min(uint(float(id.x) * float(g_gbufferWidth)  / float(g_renderWidth)),  g_gbufferWidth  - 1u),
        min(uint(float(id.y) * float(g_gbufferHeight) / float(g_renderHeight)), g_gbufferHeight - 1u));

    float4 gbufNormal = g_gbufferNormal.Load(int3(gbufPx, 0));
    // GBufferNormal.w is the LINEAR camera distance written by OpaqueGBuffer_PS.hlsl
    // (length(worldPos - cameraPos)), not an NDC depth. It used to be named ndcDepth and fed
    // straight into g_invVP as if it were one, which put the reconstructed position wherever a
    // metres-scale value lands in a [0,1] depth slot. Every other SWRT pass reconstructs with
    // cameraPos + rayDir * linearDepth; this one now matches.
    float linearDepth = gbufNormal.w;
    if (linearDepth <= 0.0f)
    {
        g_output[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    float3 N = normalize(gbufNormal.xyz * 2.0f - 1.0f);

    float ndcX =  ((float(gbufPx.x) + 0.5f) / float(g_gbufferWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(gbufPx.y) + 0.5f) / float(g_gbufferHeight)) * 2.0f + 1.0f;
    // Reverse-Z: the far plane is NDC z = 0, so this unprojects a far-plane point and the
    // camera-to-point vector gives the view ray.
    float4 viewRayH = mul(float4(ndcX, ndcY, 0.0f, 1.0f), g_invVP);
    float3 rayDir   = normalize(viewRayH.xyz / viewRayH.w - g_cameraPos);
    float3 worldPos = g_cameraPos + rayDir * linearDepth;

    if (g_tlasNodes[0].leftChild == 0 && g_tlasNodes[0].rightOrCount == 0)
    {
        g_output[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    uint sampleCount = max(1u, g_sampleCount);
    float occluded = 0.0f;

    [loop]
    for (uint s = 0u; s < sampleCount; ++s)
    {
        float2 Xi = Hammersley(s, sampleCount);
        uint hash = id.x * 1973u ^ id.y * 9277u ^ (s + g_frameIndex * 26699u);
        hash = (hash ^ (hash >> 13u)) * 1540483477u;
        hash = hash ^ (hash >> 15u);
        float2 jitter = float2(float(hash & 0xFFFFu) / 65536.0f,
                               float((hash >> 16u) & 0xFFFFu) / 65536.0f);
        Xi = frac(Xi + jitter);

        float3 sampleDir = TangentToWorld(CosineSampleHemisphere(Xi), N);
        bool blocked = TraceAnyHit(OffsetRay(worldPos, N),
                                   sampleDir,
                                   g_tMin,
                                   g_radius);
        occluded += blocked ? 1.0f : 0.0f;
    }

    float ao = 1.0f - (occluded / sampleCount);
    ao = pow(saturate(ao), max(g_power, 1e-3f));
    g_output[id.xy] = float4(ao, ao, ao, 1.0f);
}
