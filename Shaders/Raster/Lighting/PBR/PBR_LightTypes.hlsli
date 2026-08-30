#ifndef SASAMI_PBR_LIGHT_TYPES_HLSLI
#define SASAMI_PBR_LIGHT_TYPES_HLSLI

struct PointLight
{
    float4 posRange;
    float4 colorIntensity;
    // x: shadowIndex -- slice into u_pointLightVP / the cube shadow array.
    //    Negative means this light casts no shadow. yzw unused.
    float4 params;
};

struct SpotLight
{
    float4 posRange;
    float4 dirCosInner;
    float4 colorIntensity;
    // x: cosOuter
    // y: shadowIndex -- slice into u_spotLightVP / the spot shadow array.
    //    Negative means this light casts no shadow. zw unused.
    float4 params;
};

#endif // SASAMI_PBR_LIGHT_TYPES_HLSLI
