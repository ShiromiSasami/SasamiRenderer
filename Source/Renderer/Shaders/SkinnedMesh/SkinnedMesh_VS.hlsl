cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

#include "Common/LightCB.hlsli"

cbuffer BoneCB : register(b3)
{
    row_major float4x4 u_boneMatrices[128];
}

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 uv          : TEXCOORD;
    uint4  boneIndices : JOINTS_0;
    float4 boneWeights : WEIGHTS_0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
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
        return float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    }
    const float invDet = 1.0 / det;
    return float3x3(
        cofactor00 * invDet, cofactor01 * invDet, cofactor02 * invDet,
        cofactor10 * invDet, cofactor11 * invDet, cofactor12 * invDet,
        cofactor20 * invDet, cofactor21 * invDet, cofactor22 * invDet);
}

// Blend position (bone-local → model-space) using up to 4 influences
float4 SkinPosition(float3 pos, uint4 idx, float4 w)
{
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    r += w.x * mul(float4(pos, 1.0), u_boneMatrices[idx.x]);
    r += w.y * mul(float4(pos, 1.0), u_boneMatrices[idx.y]);
    r += w.z * mul(float4(pos, 1.0), u_boneMatrices[idx.z]);
    r += w.w * mul(float4(pos, 1.0), u_boneMatrices[idx.w]);
    return r;
}

// Blend normal using the upper-left 3x3 of each bone matrix
float3 SkinNormal(float3 n, uint4 idx, float4 w)
{
    float3 r = float3(0.0, 0.0, 0.0);
    r += w.x * mul(n, (float3x3)u_boneMatrices[idx.x]);
    r += w.y * mul(n, (float3x3)u_boneMatrices[idx.y]);
    r += w.z * mul(n, (float3x3)u_boneMatrices[idx.z]);
    r += w.w * mul(n, (float3x3)u_boneMatrices[idx.w]);
    return r;
}

PSInput VSMain(VSInput input)
{
    PSInput o;

    // Skinning: bone-space → model-space
    float4 skinnedPos    = SkinPosition(input.position, input.boneIndices, input.boneWeights);
    float3 skinnedNormal = SkinNormal(input.normal, input.boneIndices, input.boneWeights);

    // Model-space → world-space
    float4 worldPos = mul(skinnedPos, u_world);

    // World-space → clip-space
    o.position = mul(skinnedPos, u_mvp);

    // Normal transform (inverse-transpose of world for correct non-uniform scale)
    const float3x3 worldToObject = ComputeWorldToObject3x3();
    o.worldN = normalize(mul(skinnedNormal, worldToObject));

    o.uv       = input.uv;
    o.color    = input.color;
    o.lightPos = mul(worldPos, u_lightVP[0]);
    o.worldPos = worldPos.xyz;
    return o;
}
