// DirectX 11 native depth+normal prepass vertex shader (static, non-skinned). Same world
// transform as NativeMesh_VS.hlsl so the prepass depth matches the shaded pass's depth exactly;
// normalisation is left to the pixel shader, which is where the interpolated normal lands.
#include "NativeDepthNormalCommon.hlsli"

PSInput VSMain(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.position, 1.0), u_model);
    o.position = mul(worldPos, u_viewProjection);
    o.worldN = mul(input.normal, (float3x3)u_model);
    return o;
}
