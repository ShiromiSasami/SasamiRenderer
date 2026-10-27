// DirectX 11 shadow-caster depth-only vertex shader (skinned). Applies bone-palette linear-blend
// skinning in model space before the same light-space transform used by the static depth path.

cbuffer ShadowCB : register(b0)
{
    row_major float4x4 u_lightViewProjection;
    row_major float4x4 u_model;
}

cbuffer ShadowBoneCB : register(b1)
{
    row_major float4x4 u_boneMatrices[128];
}

struct VSInput
{
    float3 position    : POSITION;
    uint4  boneIndices : JOINTS_0;
    float4 boneWeights : WEIGHTS_0;
};

float4 SkinPosition(float3 pos, uint4 idx, float4 w)
{
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    r += w.x * mul(float4(pos, 1.0), u_boneMatrices[idx.x]);
    r += w.y * mul(float4(pos, 1.0), u_boneMatrices[idx.y]);
    r += w.z * mul(float4(pos, 1.0), u_boneMatrices[idx.z]);
    r += w.w * mul(float4(pos, 1.0), u_boneMatrices[idx.w]);
    return r;
}

float4 VSMain(VSInput input) : SV_POSITION
{
    float4 skinnedPos = SkinPosition(input.position, input.boneIndices, input.boneWeights);
    return mul(mul(skinnedPos, u_model), u_lightViewProjection);
}
