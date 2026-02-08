cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;   // not used here, but keep layout consistent
    row_major float4x4 u_world; // not used here
}

cbuffer LightCB : register(b1)
{
    row_major float4x4 u_lightVP; // not used here
    float4 u_dirDir;
    float4 u_dirColor;
    float4 u_lightCounts;
}

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

// Simple constant tess factors; tweak for quality/perf
static const float kTess = 4.0f;

HSConst PatchConstants(InputPatch<HSInput, 3> patch, uint patchId : SV_PrimitiveID)
{
    HSConst pc;
    pc.edges[0] = kTess;
    pc.edges[1] = kTess;
    pc.edges[2] = kTess;
    pc.inside   = kTess;
    return pc;
}
