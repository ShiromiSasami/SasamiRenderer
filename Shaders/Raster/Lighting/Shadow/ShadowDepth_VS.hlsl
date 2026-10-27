// ShadowDepth_VS.hlsl
// Vertex shader for depth-only shadow rendering (directional cascades, spot/point
// cube faces, and the VSM moment pass).
//
// The shadow passes used to reuse CookTorranceGGX_VS, the full G-Buffer vertex
// shader. That was measurably expensive: those passes have no pixel shader (or, for
// VSM, one that reads nothing but SV_POSITION), yet every vertex still fetched the
// 48-byte interleaved vertex and wrote 16 floats of interpolants -- worldN, uv,
// color, lightPos, worldPos -- straight into the parameter cache to be discarded.
// The shadow pass pushes ~6.6M triangles per frame (four cascades over Bistro), so
// that waste is multiplied across every one of them.
//
// This shader keeps only what a depth pass actually needs: object -> clip position.
// The input layout is narrowed to POSITION as well; the vertex buffer stride stays
// 48 bytes, so the input assembler simply fetches the first 12 of them.
//
// VSM reads clip-space depth from SV_POSITION.z (see ShadowVSM_PS.hlsl), which this
// still provides, so the same VS serves the moment pass unchanged.

cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    // Unused here, but declared so the constant buffer layout stays identical to
    // CameraCB elsewhere -- the shadow passes push camera CBs through the same
    // RendererFrameCoordinator::PushCameraCB path as every other pass.
    row_major float4x4 u_world;
}

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    // Row-vector convention (objPos * M), matching CookTorranceGGX_VS. For the
    // shadow passes u_mvp is the light's view-projection combined with the object's
    // world matrix, so this single transform is the whole job.
    o.position = mul(float4(input.position, 1.0), u_mvp);
    return o;
}
