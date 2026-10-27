// VolumetricCloud_VS.hlsl
// Fullscreen triangle at z = 0.0 (far plane under reverse-Z) so the depth test
// (D3D12_COMPARISON_FUNC_GREATER_EQUAL, no depth write) naturally passes
// only on sky/background pixels (depth == 0.0) and is rejected by opaque
// geometry pixels (depth > 0.0).  No vertex buffer is required.

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    // Generate a fullscreen triangle from three vertices using bit-tricks:
    //   id=0 → (-1, -1)
    //   id=1 → ( 3, -1)
    //   id=2 → (-1,  3)
    float2 uv  = float2((id << 1u) & 2u, id & 2u);
    float2 ndc = uv * 2.0f - 1.0f;

    VSOutput o;
    o.position = float4(ndc.x, -ndc.y, 0.0f, 1.0f); // reverse-Z: z=0 → far plane
    o.uv       = uv;
    return o;
}
