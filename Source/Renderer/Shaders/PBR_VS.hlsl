cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

#include "Common/LightCB.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1; // clip-space in light view
    float3 worldPos : TEXCOORD2;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    // Object -> world transform for position.
    float4 worldPos = mul(float4(input.position, 1.0), u_world);
    // Object -> clip transform.
    o.position = mul(float4(input.position, 1.0), u_mvp);
    // Normal uses w=0 so translation component in u_world is ignored.
    o.worldN   = normalize(mul(float4(input.normal, 0.0), u_world).xyz);
    o.uv       = input.uv;
    o.color    = input.color;
    // World -> light clip transform for shadow lookup.
    o.lightPos = mul(worldPos, u_lightVP);
    o.worldPos = worldPos.xyz;
    return o;
}
