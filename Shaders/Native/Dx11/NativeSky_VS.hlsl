// NativeSky_VS.hlsl -- fullscreen triangle for the native-path sky.
//
// No vertex buffer: the three positions come from the vertex id, the standard trick for a
// fullscreen pass. A triangle rather than a quad because a quad's diagonal makes the two halves
// shade in separate wavefronts along the seam.

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput o;
    // (0,0), (2,0), (0,2) in UV -- the triangle overhangs the screen on two sides, and the
    // rasteriser clips it, which is cheaper than two triangles sharing an edge.
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}
