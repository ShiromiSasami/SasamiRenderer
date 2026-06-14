// Full-screen triangle vertex shader for SSAO pass.
// No vertex buffer required; triangle is generated from SV_VertexID.
//
// Triangle coverage:
//   id=0  -> NDC(-1, 1),  UV(0, 0)  [top-left]
//   id=1  -> NDC( 3, 1),  UV(2, 0)  [far right, clips to right edge]
//   id=2  -> NDC(-1,-3),  UV(0, 2)  [far bottom, clips to bottom edge]
//
// The three vertices form a triangle that covers the entire NDC clip square.

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    // UV: (0,0) = top-left, (1,1) = bottom-right
    float2 uv = float2((id == 1u) ? 2.0f : 0.0f,
                       (id == 2u) ? 2.0f : 0.0f);
    VSOut o;
    // NDC: x = uv.x * 2 - 1, y = 1 - uv.y * 2  (flip Y for D3D12 convention)
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    o.uv  = uv;
    return o;
}
