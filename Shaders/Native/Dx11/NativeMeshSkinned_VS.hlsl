// DirectX 11 native mesh vertex shader (skinned). Performs bone-palette linear-blend skinning in
// model space, then applies the same u_model/u_viewProjection transform as the static native mesh
// path. PSInput is field-for-field identical to the static path's, so NativeMesh_PS.hlsl is reused
// unmodified (no skinned-specific pixel shader).
#include "NativeMeshCommon.hlsli"

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
    o.worldPos = worldPos.xyz;
    o.worldN = normalize(mul(skinnedNormal, (float3x3)u_model));
    o.color = input.color * u_baseColor;
    o.uv = input.uv;
    return o;
}
