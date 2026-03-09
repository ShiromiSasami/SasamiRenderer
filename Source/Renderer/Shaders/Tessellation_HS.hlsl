cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;   // not used here, but keep layout consistent
    row_major float4x4 u_world; // not used here
}

#include "Common/LightCB.hlsli"

struct HSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct HSConst
{
    float edges[3]  : SV_TessFactor;
    float inside    : SV_InsideTessFactor;
};

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstants")] 
HSInput HSMain(InputPatch<HSInput, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}

// Constant tessellation factor per edge/inside.
// Larger value => more generated triangles (higher quality, higher cost).
static const float kTess = 4.0f;

HSConst PatchConstants(InputPatch<HSInput, 3> patch, uint patchId : SV_PrimitiveID)
{
    HSConst pc;
    // Isotropic tessellation: all three edges and inside share same factor.
    pc.edges[0] = kTess;
    pc.edges[1] = kTess;
    pc.edges[2] = kTess;
    pc.inside   = kTess;
    return pc;
}
