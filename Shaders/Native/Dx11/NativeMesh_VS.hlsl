// DirectX 11 native mesh vertex shader (static, non-skinned). Transforms position and normal into
// world space, then projects. Same transform as the embedded kDx11MeshVertexShader it replaces.
#include "NativeMeshCommon.hlsli"

PSInput VSMain(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.position, 1.0), u_model);
    o.position = mul(worldPos, u_viewProjection);
    o.worldPos = worldPos.xyz;
    o.worldN = normalize(mul(input.normal, (float3x3)u_model));
    o.color = input.color * u_baseColor;
    o.uv = input.uv;
    return o;
}
