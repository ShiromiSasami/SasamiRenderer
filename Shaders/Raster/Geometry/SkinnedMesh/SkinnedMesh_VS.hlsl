cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

#include "Shared/Common/LightCB.hlsli"

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

#include "Shared/Common/NormalMatrix.hlsli"

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
    // Matrix-first: see the note in CookTorranceGGX_VS.hlsl -- with row-vector
    // positions the normal needs transpose(inverse(W)), not inverse(W).
    o.worldN = normalize(mul(worldToObject, skinnedNormal));

    o.uv       = input.uv;
    o.color    = input.color;
    o.lightPos = mul(worldPos, u_lightVP[0]);
    o.worldPos = worldPos.xyz;
    return o;
}
