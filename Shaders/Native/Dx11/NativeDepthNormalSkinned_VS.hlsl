// DirectX 11 native depth+normal prepass vertex shader (skinned). Bone-palette linear-blend
// skinning in model space, then the same transform as the static variant -- mirroring how
// NativeMeshSkinned_VS.hlsl relates to NativeMesh_VS.hlsl.
#include "NativeDepthNormalCommon.hlsli"

float4 SkinPosition(float3 pos, uint4 idx, float4 w)
{
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    r += w.x * mul(float4(pos, 1.0), u_boneMatrices[idx.x]);
    r += w.y * mul(float4(pos, 1.0), u_boneMatrices[idx.y]);
    r += w.z * mul(float4(pos, 1.0), u_boneMatrices[idx.z]);
    r += w.w * mul(float4(pos, 1.0), u_boneMatrices[idx.w]);
    return r;
}

float3 SkinNormal(float3 n, uint4 idx, float4 w)
{
    float3 r = float3(0.0, 0.0, 0.0);
    r += w.x * mul(n, (float3x3)u_boneMatrices[idx.x]);
    r += w.y * mul(n, (float3x3)u_boneMatrices[idx.y]);
    r += w.z * mul(n, (float3x3)u_boneMatrices[idx.z]);
    r += w.w * mul(n, (float3x3)u_boneMatrices[idx.w]);
    return r;
}

PSInput VSMain(SkinnedVSInput input)
{
    PSInput o;
    float4 skinnedPos = SkinPosition(input.position, input.boneIndices, input.boneWeights);
    float3 skinnedNormal = SkinNormal(input.normal, input.boneIndices, input.boneWeights);
    float4 worldPos = mul(skinnedPos, u_model);
    o.position = mul(worldPos, u_viewProjection);
    o.worldN = mul(skinnedNormal, (float3x3)u_model);
    return o;
}
