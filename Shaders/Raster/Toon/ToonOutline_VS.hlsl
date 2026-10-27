// ToonOutline_VS.hlsl
// Vertex shader for the toon inverted-hull outline pass. Pushes each vertex out
// along its world-space normal by a fixed world distance, so the back faces of the
// pushed-out hull render as a silhouette line around the mesh when rasterized with
// front-face culling.
//
// The vertex input layout matches the opaque G-Buffer pass exactly (same vertex
// buffer, same stride) so this shader can share the geometry draw calls.

cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

#include "Shared/Common/LightCB.hlsli"
#include "Shared/Common/NormalMatrix.hlsli"

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

    // Zero (or negative) width means outlines are disabled. This pass is always
    // part of the draw sequence, so rather than gating it from the CPU side, throw
    // every vertex outside the clip volume here -- the resulting triangles are
    // degenerate and produce no pixels.
    if (u_toonOutline.x <= 0.0)
    {
        o.position = float4(0.0, 0.0, -2.0, 1.0);
        return o;
    }

    const float3x3 worldToObject = ComputeWorldToObject3x3();
    float4 worldPos = mul(float4(input.position, 1.0), u_world);
    // See NormalMatrix.hlsli: matrix-first mul() applies transpose(inverse(W)) to
    // match the row-vector position transform above.
    float3 worldN = normalize(mul(worldToObject, input.normal));

    // Push along the world normal by a fixed world-space distance rather than a
    // screen-space one: scaling with depth would keep the line the same number of
    // pixels wide, but the line would then not track the scene's physical scale
    // and would grow disproportionately thick on distant geometry. A fixed world
    // width stays visually consistent with the object it outlines regardless of
    // camera distance.
    worldPos.xyz += worldN * u_toonOutline.x;

    o.position = mul(worldPos, u_cameraPV);
    return o;
}
