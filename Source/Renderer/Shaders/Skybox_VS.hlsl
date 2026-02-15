cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

struct VSInput
{
    float3 position : POSITION;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 dir      : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    // Transform skybox cube vertex to clip space.
    float4 clip = mul(float4(input.position, 1.0), u_mvp);
    // Set z=w so depth becomes 1.0 after perspective divide:
    // z_ndc = z / w = 1.0 (far plane), keeping sky behind scene geometry.
    o.position = clip.xyww; // force depth to far plane so skybox stays behind scene geometry
    // Direction for cubemap lookup (camera-centered skybox).
    o.dir = input.position;
    return o;
}
