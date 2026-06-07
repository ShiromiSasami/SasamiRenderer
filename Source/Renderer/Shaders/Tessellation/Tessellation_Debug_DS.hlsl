cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

#include "../Common/LightCB.hlsli"

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
    float3 patchDebugColor : COLOR1;
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

float3x3 ComputeWorldToObject3x3()
{
    const float a = u_world[0][0]; const float b = u_world[0][1]; const float c = u_world[0][2];
    const float d = u_world[1][0]; const float e = u_world[1][1]; const float f = u_world[1][2];
    const float g = u_world[2][0]; const float h = u_world[2][1]; const float i = u_world[2][2];

    const float cofactor00 = e*i - f*h; const float cofactor01 = c*h - b*i; const float cofactor02 = b*f - c*e;
    const float cofactor10 = f*g - d*i; const float cofactor11 = a*i - c*g; const float cofactor12 = c*d - a*f;
    const float cofactor20 = d*h - e*g; const float cofactor21 = b*g - a*h; const float cofactor22 = a*e - b*d;

    const float det = a * cofactor00 + b * cofactor10 + c * cofactor20;
    if (abs(det) <= 1e-8f)
        return float3x3(1,0,0, 0,1,0, 0,0,1);

    const float inv = 1.0f / det;
    return float3x3(
        cofactor00*inv, cofactor01*inv, cofactor02*inv,
        cofactor10*inv, cofactor11*inv, cofactor12*inv,
        cofactor20*inv, cofactor21*inv, cofactor22*inv);
}

float3 PhongProject(float3 p, float3 pv, float3 nv)
{
    return p - dot(p - pv, nv) * nv;
}

static const float kPhongAlpha = 0.65f;

[domain("tri")]
PSInput DSMain(
    const HSConst HSConstData,
    float3 bary : SV_DomainLocation,
    const OutputPatch<HSInput, 3> patch)
{
    float3 p0 = patch[0].position; float3 p1 = patch[1].position; float3 p2 = patch[2].position;
    float3 n0 = patch[0].normal;   float3 n1 = patch[1].normal;   float3 n2 = patch[2].normal;
    float2 uv0 = patch[0].uv;      float2 uv1 = patch[1].uv;      float2 uv2 = patch[2].uv;

    float3 posLin = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    float3 q0 = PhongProject(posLin, p0, normalize(n0));
    float3 q1 = PhongProject(posLin, p1, normalize(n1));
    float3 q2 = PhongProject(posLin, p2, normalize(n2));
    float3 posPhong = q0 * bary.x + q1 * bary.y + q2 * bary.z;
    float3 pos = lerp(posLin, posPhong, kPhongAlpha);

    float3 nor = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    float2 uv  = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;
    float4 col = float4(HSConstData.patchDebugColor, 1.0f);

    const float3x3 worldToObject = ComputeWorldToObject3x3();
    float4 worldPos4 = mul(float4(pos, 1.0f), u_world);

    PSInput o;
    o.position = mul(float4(pos, 1.0f), u_mvp);
    o.worldN   = normalize(mul(nor, worldToObject));
    o.uv       = uv;
    o.color    = col;
    o.lightPos = mul(worldPos4, u_lightVP[0]);
    o.worldPos = worldPos4.xyz;
    return o;
}
