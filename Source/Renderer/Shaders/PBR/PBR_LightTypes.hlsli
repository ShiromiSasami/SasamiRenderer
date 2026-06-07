#ifndef SASAMI_PBR_LIGHT_TYPES_HLSLI
#define SASAMI_PBR_LIGHT_TYPES_HLSLI

struct PointLight
{
    float4 posRange;
    float4 colorIntensity;
};

struct SpotLight
{
    float4 posRange;
    float4 dirCosInner;
    float4 colorIntensity;
    float4 params; // x: cosOuter
};

#endif // SASAMI_PBR_LIGHT_TYPES_HLSLI
