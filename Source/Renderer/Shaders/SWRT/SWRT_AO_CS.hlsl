#include "SWRT/SWRT_Common.hlsli"

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

float3 TangentToWorld(float3 v, float3 N)
{
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    return normalize(T * v.x + B * v.y + N * v.z);
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
    float ndcDepth = gbufNormal.w;
    if (ndcDepth <= 0.0f)
    {
        g_output[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    float3 N = normalize(gbufNormal.xyz * 2.0f - 1.0f);

    float ndcX =  ((float(gbufPx.x) + 0.5f) / float(g_gbufferWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(gbufPx.y) + 0.5f) / float(g_gbufferHeight)) * 2.0f + 1.0f;
    float4 posH = mul(float4(ndcX, ndcY, ndcDepth, 1.0f), g_invVP);
    float3 worldPos = posH.xyz / max(posH.w, 1e-6f);

    if (g_tlasNodes[0].leftChild == 0 && g_tlasNodes[0].rightOrCount == 0)
    {
        g_output[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    uint sampleCount = max(1u, g_sampleCount);
    float occluded = 0.0f;
    const float originBias = max(g_tMin, 1e-4f);

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
        bool blocked = TraceAnyHit(worldPos + N * originBias,
                                   sampleDir,
                                   originBias,
                                   g_radius);
        occluded += blocked ? 1.0f : 0.0f;
    }

    float ao = 1.0f - (occluded / sampleCount);
    ao = pow(saturate(ao), max(g_power, 1e-3f));
    g_output[id.xy] = float4(ao, ao, ao, 1.0f);
}
