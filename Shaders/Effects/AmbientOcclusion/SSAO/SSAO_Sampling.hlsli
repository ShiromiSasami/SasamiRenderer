#ifndef SASAMI_SSAO_SAMPLING_HLSLI
#define SASAMI_SSAO_SAMPLING_HLSLI

// SSAO kernel, reconstruction, normal decode, and sampling helpers.

static const float PI = 3.14159265f;
static const uint  kMaxKernelSamples = 32u;

static const float3 g_ssaoKernel[kMaxKernelSamples] =
{
    float3( 0.1340f,  0.0000f,  0.9910f),
    float3( 0.4640f, -0.2130f,  0.8600f),
    float3(-0.1820f,  0.5010f,  0.8470f),
    float3(-0.5570f, -0.2040f,  0.8060f),
    float3( 0.5410f,  0.4370f,  0.7190f),
    float3( 0.0420f, -0.7440f,  0.6670f),
    float3(-0.6810f,  0.3180f,  0.6600f),
    float3( 0.5170f, -0.5500f,  0.6560f),
    float3(-0.6510f, -0.4590f,  0.6050f),
    float3( 0.7600f,  0.2010f,  0.6180f),
    float3(-0.0680f,  0.7960f,  0.6010f),
    float3(-0.2740f, -0.7510f,  0.6000f),
    float3( 0.7280f, -0.4640f,  0.5050f),
    float3(-0.7820f,  0.3530f,  0.5150f),
    float3( 0.2680f,  0.8200f,  0.5060f),
    float3(-0.5590f, -0.6440f,  0.5220f),
    float3( 0.8890f,  0.0110f,  0.4580f),
    float3( 0.6610f,  0.6170f,  0.4270f),
    float3(-0.0720f,  0.9200f,  0.3850f),
    float3(-0.7070f,  0.6200f,  0.3400f),
    float3(-0.9540f, -0.0410f,  0.2980f),
    float3(-0.6120f, -0.7610f,  0.2140f),
    float3( 0.1130f, -0.9820f,  0.1510f),
    float3( 0.7740f, -0.6180f,  0.1360f),
    float3( 0.9840f,  0.0850f,  0.1560f),
    float3( 0.5430f,  0.8340f,  0.0970f),
    float3(-0.1860f,  0.9800f,  0.0740f),
    float3(-0.8220f,  0.5660f,  0.0610f),
    float3(-0.9980f, -0.0110f,  0.0620f),
    float3(-0.5610f, -0.8280f,  0.0210f),
    float3( 0.2150f, -0.9750f,  0.0520f),
    float3( 0.8320f, -0.5510f,  0.0560f)
};

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f,
                        (1.0f - uv.y) * 2.0f - 1.0f,
                        depth,
                        1.0f);
    float4 worldH = mul(ndc, u_cameraInvPV);
    return worldH.xyz / max(worldH.w, 1e-6f);
}

float3 DecodeNormal(float2 uv)
{
    float3 encoded = NormalTex.SampleLevel(PointClamp, uv, 0).xyz;
    float3 normal = encoded * 2.0f - 1.0f;
    float lenSq = dot(normal, normal);
    if (lenSq <= 1e-5f) {
        return float3(0.0f, 1.0f, 0.0f);
    }
    return normalize(normal);
}

float Hash12(float2 p)
{
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453123f);
}

float3 BuildTangent(float3 normal, float angle)
{
    float3 randDir = float3(cos(angle), sin(angle), 0.0f);
    float3 tangent = randDir - normal * dot(randDir, normal);
    float lenSq = dot(tangent, tangent);
    if (lenSq <= 1e-5f) {
        tangent = abs(normal.z) < 0.999f ? cross(normal, float3(0.0f, 0.0f, 1.0f))
                                         : cross(normal, float3(0.0f, 1.0f, 0.0f));
        lenSq = dot(tangent, tangent);
    }
    return tangent * rsqrt(max(lenSq, 1e-6f));
}

uint ResolveSampleCount(uint quality)
{
    return (quality == 0u) ? 12u : (quality == 1u) ? 20u : 32u;
}

float ResolveSampleRadius(uint sampleIndex, uint sampleCount)
{
    float normalized = (float(sampleIndex) + 0.5f) / float(sampleCount);
    return lerp(0.12f, 1.0f, normalized * normalized);
}

#endif // SASAMI_SSAO_SAMPLING_HLSLI
