struct Particle
{
    float3 position;
    float  life;
    float3 velocity;
    float  maxLife;
    float4 color;
    float  size;
    float3 pad;
};

cbuffer CameraCB : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 world;
    float4 extra0;
    float4 extra1;
    float4 extra2;
};

StructuredBuffer<Particle> g_particles : register(t0);

struct VSOutput
{
    float4 svPosition : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

static const float2 kCorners[4] =
{
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2(-1.0,  1.0),
    float2( 1.0,  1.0),
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    VSOutput o;
    Particle p = g_particles[instanceId];

    if (p.life <= 0.0)
    {
        o.svPosition = float4(0, 0, -2, 1);
        o.uv = float2(0, 0);
        o.color = float4(0, 0, 0, 0);
        return o;
    }

    float2 corner = kCorners[vertexId];
    float3 right = extra0.xyz;
    float3 up = extra1.xyz;
    float3 worldPos = p.position + (right * corner.x + up * corner.y) * (p.size * 0.5);

    o.svPosition = mul(float4(worldPos, 1.0), viewProj);
    o.uv = corner * 0.5 + 0.5;
    o.color = p.color;
    return o;
}
