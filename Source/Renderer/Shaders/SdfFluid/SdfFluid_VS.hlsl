// SdfFluid_VS.hlsl
// Fullscreen triangle from SV_VertexID — no vertex buffer, no input layout.
// Outputs SV_POSITION only; the PS reconstructs world-space ray direction from pixel coords.

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut VSMain(uint id : SV_VertexID)
{
    // Three vertices that form a triangle covering the entire screen:
    //   id=0 -> (-1, -1)
    //   id=1 -> ( 3, -1)
    //   id=2 -> (-1,  3)
    float2 ndc;
    ndc.x = (id == 1) ?  3.0f : -1.0f;
    ndc.y = (id == 2) ?  3.0f : -1.0f;

    VSOut o;
    o.pos = float4(ndc, 0.0f, 1.0f); // z=0 (near plane), w=1
    return o;
}
