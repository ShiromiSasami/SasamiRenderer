// RayMarch_VS.hlsl
// Fullscreen triangle from SV_VertexID — no vertex buffer, no input layout.
// Identical pattern to SdfFluid_VS.hlsl.

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut VSMain(uint id : SV_VertexID)
{
    float2 ndc;
    ndc.x = (id == 1) ?  3.0f : -1.0f;
    ndc.y = (id == 2) ?  3.0f : -1.0f;

    VSOut o;
    o.pos = float4(ndc, 0.0f, 1.0f);
    return o;
}
