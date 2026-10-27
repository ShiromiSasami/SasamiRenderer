// Wireframe_VS.hlsl
// Vertex shader for the editor wireframe overlay (view mode "Wireframe"). Transforms the
// opaque vertex stream exactly like the main G-Buffer VS, so the rasterizer's wireframe
// fill draws the edges of the triangles the scene already shows.
//
// The vertex input layout matches the opaque G-Buffer pass (same vertex buffer and stride)
// so this shader can share its draw calls.

cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = mul(float4(input.position, 1.0), u_mvp);
    return o;
}
