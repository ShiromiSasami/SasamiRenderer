cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

cbuffer LightCB : register(b1)
{
    row_major float4x4 u_lightVP;
    float4 u_dirDir;
    float4 u_dirColor;
    float4 u_lightCounts;
    float4 u_cameraPos;
    float4 u_iblParams;
    float4 u_debugParams;
}

struct HSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

// Must match Hull Shader patch constant type
struct HSConst
{
    float edges[3]  : SV_TessFactor;
    float inside    : SV_InsideTessFactor;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

[domain("tri")]
PSInput DSMain(
    const HSConst HSConstData,
    float3 bary : SV_DomainLocation,
    const OutputPatch<HSInput, 3> patch)
{
    // Barycentric interpolation on triangle:
    // value = v0*bary.x + v1*bary.y + v2*bary.z
    // where bary.x + bary.y + bary.z = 1.
    float3 p0 = patch[0].position;
    float3 p1 = patch[1].position;
    float3 p2 = patch[2].position;
    float3 n0 = patch[0].normal;
    float3 n1 = patch[1].normal;
    float3 n2 = patch[2].normal;
    float2 uv0 = patch[0].uv;
    float2 uv1 = patch[1].uv;
    float2 uv2 = patch[2].uv;
    float4 c0 = patch[0].color;
    float4 c1 = patch[1].color;
    float4 c2 = patch[2].color;

    float3 pos = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    // Normalized interpolated normal to keep unit length after interpolation.
    float3 nor = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    float2 uv  = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;
    float4 col = c0 * bary.x + c1 * bary.y + c2 * bary.z;

    PSInput o;
    // Object -> world/clip/light-clip transforms after tessellation.
    float4 worldPos = mul(float4(pos, 1.0), u_world);
    o.position = mul(float4(pos, 1.0), u_mvp);
    o.worldN   = normalize(mul(float4(nor, 0.0), u_world).xyz);
    o.uv       = uv;
    o.color    = col;
    o.lightPos = mul(worldPos, u_lightVP);
    o.worldPos = worldPos.xyz;
    return o;
}
