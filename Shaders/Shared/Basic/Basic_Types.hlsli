#ifndef SASAMI_BASIC_TYPES_HLSLI
#define SASAMI_BASIC_TYPES_HLSLI

// Basic forward shader light and vertex/pixel interface types.

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

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float4 lightPos : TEXCOORD1; // clip-space in light view
    float3 worldPos : TEXCOORD2;
};

#endif // SASAMI_BASIC_TYPES_HLSLI
