#ifndef SWRT_COMMON_HLSLI
#define SWRT_COMMON_HLSLI

// ---------------------------------------------------------------------------
// GPU Software Ray Tracer shared interface.
// Keep this file as the public include point. Responsibility-specific code is
// split into the files below so entry shaders do not depend on one monolithic
// common header.
// ---------------------------------------------------------------------------

#include "SWRT_Types.hlsli"
#include "SWRT_Material.hlsli"
#include "SWRT_Bindings.hlsli"
#include "SWRT_Intersection.hlsli"
#include "SWRT_Traversal.hlsli"

// Transform a tangent-space vector v into world space aligned with normal N.
float3 TangentToWorld(float3 v, float3 N)
{
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    return normalize(T * v.x + B * v.y + N * v.z);
}

// GGX normal distribution function.
float GGX_D(float NdotH, float roughness)
{
    // Floor alpha (Filament MIN_PERCEPTUAL_ROUGHNESS 0.045): roughness 0 gave 0/0 at NdotH = 1.
    float a  = max(roughness * roughness, 0.002025f);
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d);
}

// Smith GGX visibility (approximation).
float GGX_V(float NdotL, float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gL = NdotL / (NdotL * (1.0f - k) + k);
    float gV = NdotV / (NdotV * (1.0f - k) + k);
    return gL * gV;
}

#endif // SWRT_COMMON_HLSLI
