// DirectX 11 shadow-caster depth-only vertex shader (static, non-skinned). Only position is
// needed since the shadow map has no pixel shader output beyond depth.

cbuffer ShadowCB : register(b0)
{
    row_major float4x4 u_lightViewProjection;
    row_major float4x4 u_model;
}

struct VSInput
{
    float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_POSITION
{
    return mul(mul(float4(input.position, 1.0), u_model), u_lightViewProjection);
}
