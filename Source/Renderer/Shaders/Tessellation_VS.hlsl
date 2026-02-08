cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;   // used in DS
    row_major float4x4 u_world; // used in DS
}

cbuffer LightCB : register(b1)
{
    row_major float4x4 u_lightVP; // used in DS
    float4 u_dirDir;
    float4 u_dirColor;
    float4 u_lightCounts;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

// Control point data passed to Hull Shader
struct HSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

HSInput VSMain(VSInput i)
{
    HSInput o;
    // Keep data in object space; DS will transform by u_mvp/u_lightVP
    o.position = i.position;
    o.normal   = i.normal;
    o.color    = i.color;
    o.uv       = i.uv;
    return o;
}
