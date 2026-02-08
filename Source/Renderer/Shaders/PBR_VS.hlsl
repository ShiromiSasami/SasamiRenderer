cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

cbuffer LightCB : register(b1)
{
    row_major float4x4 u_lightVP;
    float4 u_dirDir;    // xyz: forward dir, w: intensity
    float4 u_dirColor;  // rgb: color
    float4 u_lightCounts; // x: pointCount, y: spotCount
}

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

PSInput VSMain(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.position, 1.0), u_world);
    o.position = mul(float4(input.position, 1.0), u_mvp);
    o.worldN   = normalize(mul(float4(input.normal, 0.0), u_world).xyz);
    o.uv       = input.uv;
    o.color    = input.color;
    o.lightPos = mul(worldPos, u_lightVP);
    o.worldPos = worldPos.xyz;
    return o;
}
