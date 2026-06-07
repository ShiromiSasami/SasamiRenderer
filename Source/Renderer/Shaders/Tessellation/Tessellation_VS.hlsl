cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;   // used in DS
    row_major float4x4 u_world; // used in DS
}

#include "Common/LightCB.hlsli"

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
    // Keep data in object space.
    // Tessellation happens before final transform so DS can interpolate first,
    // then apply object/world/clip transforms on tessellated vertices.
    o.position = i.position;
    o.normal   = i.normal;
    o.color    = i.color;
    o.uv       = i.uv;
    return o;
}
