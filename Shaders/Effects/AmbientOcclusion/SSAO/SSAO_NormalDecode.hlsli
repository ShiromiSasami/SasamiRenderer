#ifndef SASAMI_SSAO_NORMAL_DECODE_HLSLI
#define SASAMI_SSAO_NORMAL_DECODE_HLSLI

// Shared view-space normal decode math for the SSAO main and blur passes.
// Texture/sampler sampling stays per-file; only the post-sample remap is shared.

float3 DecodeNormalFromEncoded(float3 encoded)
{
    float3 normal = encoded * 2.0f - 1.0f;
    float lenSq = dot(normal, normal);
    if (lenSq <= 1e-5f) {
        return float3(0.0f, 1.0f, 0.0f);
    }
    return normalize(normal);
}

#endif
