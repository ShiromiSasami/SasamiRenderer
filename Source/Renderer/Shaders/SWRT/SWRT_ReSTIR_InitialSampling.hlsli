#ifndef SASAMI_SWRT_RESTIR_INITIAL_SAMPLING_HLSLI
#define SASAMI_SWRT_RESTIR_INITIAL_SAMPLING_HLSLI

// Initial ReSTIR reflection ray sampling and candidate-light helpers.

float3 ComputeCameraRayDir(uint2 pixel, uint2 sourceSize)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(sourceSize.x)) * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(sourceSize.y)) * 2.0f + 1.0f;
    float4 dir = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    return normalize(dir.xyz / dir.w - g_cameraPos);
}

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float3 TangentToWorld(float3 v, float3 N)
{
    float3 up = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);
    return normalize(T * v.x + B * v.y + N * v.z);
}

float3 SampleGGX_H(float2 xi, float roughness)
{
    float a = max(roughness * roughness, 0.0001f);
    float phi = 6.28318530718f * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / max(1.0f + (a * a - 1.0f) * xi.y, 1e-5f));
    float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

uint TotalLightCount() { return g_pointLightCount + g_spotLightCount; }

float EvalPhat(uint i, float3 pos, float3 N)
{
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
    float spotAtten = smoothstep(sl.cosOuter, sl.cosInner, cosA);
    return PhatPoint(pos, N, sl.pos, sl.colorIntensity * spotAtten, sl.range);
}

void StoreInvalid(uint2 pixel, uint pixIdx)
{
    g_gbufferOut[pixel]     = float4(0.0f, 0.0f, 0.0f, -1.0f);
    g_hitPositionOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_hitMaterialOut[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_reservoirOut[pixIdx]  = InitReservoir();
}

#endif // SASAMI_SWRT_RESTIR_INITIAL_SAMPLING_HLSLI
