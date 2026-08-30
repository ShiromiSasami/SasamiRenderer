cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

#include "Shared/Common/LightCB.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1; // clip-space in light view
    float3 worldPos : TEXCOORD2;
};

#include "Shared/Common/NormalMatrix.hlsli"

PSInput VSMain(VSInput input)
{
    PSInput o;
    const float3x3 worldToObject = ComputeWorldToObject3x3();
    // Object -> world transform for position.
    float4 worldPos = mul(float4(input.position, 1.0), u_world);
    // Object -> clip transform.
    o.position = mul(float4(input.position, 1.0), u_mvp);
    // Normal transform for the row-vector convention used by the position above
    // (worldPos = objPos * u_world): n_world = n_obj * transpose(inverse(W)).
    // mul(matrix, vector) applies the transpose of the row-vector product, so the
    // matrix must come first here. mul(normal, worldToObject) would instead apply
    // inverse(W) directly, which rotates normals the wrong way on any rotated mesh.
    o.worldN   = normalize(mul(worldToObject, input.normal));
    o.uv       = input.uv;
    o.color    = input.color;
    // World -> light clip transform for shadow lookup.
    o.lightPos = mul(worldPos, u_lightVP[0]);
    o.worldPos = worldPos.xyz;
    return o;
}
