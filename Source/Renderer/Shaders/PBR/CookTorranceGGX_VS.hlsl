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

float3x3 ComputeWorldToObject3x3()
{
    const float a = u_world[0][0];
    const float b = u_world[0][1];
    const float c = u_world[0][2];
    const float d = u_world[1][0];
    const float e = u_world[1][1];
    const float f = u_world[1][2];
    const float g = u_world[2][0];
    const float h = u_world[2][1];
    const float i = u_world[2][2];

    const float cofactor00 = e * i - f * h;
    const float cofactor01 = c * h - b * i;
    const float cofactor02 = b * f - c * e;
    const float cofactor10 = f * g - d * i;
    const float cofactor11 = a * i - c * g;
    const float cofactor12 = c * d - a * f;
    const float cofactor20 = d * h - e * g;
    const float cofactor21 = b * g - a * h;
    const float cofactor22 = a * e - b * d;

    const float det = a * cofactor00 + b * cofactor10 + c * cofactor20;
    if (abs(det) <= 1e-8) {
        return float3x3(
            1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0);
    }

    const float invDet = 1.0 / det;
    return float3x3(
        cofactor00 * invDet, cofactor01 * invDet, cofactor02 * invDet,
        cofactor10 * invDet, cofactor11 * invDet, cofactor12 * invDet,
        cofactor20 * invDet, cofactor21 * invDet, cofactor22 * invDet);
}

PSInput VSMain(VSInput input)
{
    PSInput o;
    const float3x3 worldToObject = ComputeWorldToObject3x3();
    // Object -> world transform for position.
    float4 worldPos = mul(float4(input.position, 1.0), u_world);
    // Object -> clip transform.
    o.position = mul(float4(input.position, 1.0), u_mvp);
    // Transform normal with inverse(world) for row-vector convention.
    o.worldN   = normalize(mul(input.normal, worldToObject));
    o.uv       = input.uv;
    o.color    = input.color;
    // World -> light clip transform for shadow lookup.
    o.lightPos = mul(worldPos, u_lightVP[0]);
    o.worldPos = worldPos.xyz;
    return o;
}
