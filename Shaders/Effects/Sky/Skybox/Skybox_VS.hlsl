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
    // Reverse-Z: far plane is z_ndc = 0.0, not 1.0. Force NDC z to 0 directly (instead of
    // z=w) so depth becomes 0.0 after perspective divide, keeping sky behind scene geometry.
    o.position = float4(clip.xy, 0.0f, clip.w); // force depth to far plane so skybox stays behind scene geometry
    // Direction for cubemap lookup (camera-centered skybox).
    o.dir = input.position;
    return o;
}
