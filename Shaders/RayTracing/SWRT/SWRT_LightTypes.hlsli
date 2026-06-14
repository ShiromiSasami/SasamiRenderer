#ifndef SASAMI_SWRT_LIGHT_TYPES_HLSLI
#define SASAMI_SWRT_LIGHT_TYPES_HLSLI

// Shared point/spot light layout used by SWRT reflection and ReSTIR passes.

struct GpuPointLightRT
{
    float3 pos;
    float range;
    float3 colorIntensity;
    float pad;
};

struct GpuSpotLightRT
{
    float3 pos;
    float range;
    float3 dir;
    float cosInner;
    float3 colorIntensity;
    float cosOuter;
};

#endif // SASAMI_SWRT_LIGHT_TYPES_HLSLI
