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

cbuffer ParticleUpdateCB : register(b0)
{
    float    g_deltaTime;
    float    g_gravity;
    float    g_drag;
    uint     g_capacity;

    uint     g_emitCursor;
    uint     g_emitCount;
    float    g_lifeMin;
    float    g_lifeMax;

    float    g_sizeMin;
    float    g_sizeMax;
    float    g_emitSpeed;
    float    g_emitSpread;

    float    g_emitOriginX;
    float    g_emitOriginY;
    float    g_emitOriginZ;
    float    g_emitRadius;

    float4   g_colorStart;
    float4   g_colorEnd;

    uint     g_frameSeed;
    float3   g_pad1;
};

RWStructuredBuffer<Particle> g_particles : register(u0);

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float RandFloat(inout uint state)
{
    state = Hash(state);
    return float(state) / 4294967296.0;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= g_capacity) return;

    uint state = Hash(i ^ g_frameSeed);
    uint rel = (i + g_capacity - g_emitCursor) % g_capacity;
    bool emit = rel < g_emitCount;

    Particle p = g_particles[i];

    if (emit)
    {
        float a0 = RandFloat(state) * 6.2831853;
        float r0 = sqrt(RandFloat(state)) * g_emitRadius;
        p.position = float3(g_emitOriginX + cos(a0) * r0,
                             g_emitOriginY,
                             g_emitOriginZ + sin(a0) * r0);

        float coneAngle = g_emitSpread * 1.5707963;
        float a1 = RandFloat(state) * 6.2831853;
        float r1 = RandFloat(state) * coneAngle;
        float sinR = sin(r1);
        float3 dir = float3(cos(a1) * sinR, cos(r1), sin(a1) * sinR);

        p.velocity = dir * g_emitSpeed;
        p.maxLife = lerp(g_lifeMin, g_lifeMax, RandFloat(state));
        p.life = p.maxLife;
        p.size = lerp(g_sizeMin, g_sizeMax, RandFloat(state));
        p.color = g_colorStart;
    }
    else if (p.life > 0.0)
    {
        p.velocity.y += g_gravity * g_deltaTime;
        p.velocity *= max(1.0 - g_drag * g_deltaTime, 0.0);
        p.position += p.velocity * g_deltaTime;
        p.life -= g_deltaTime;

        float t = p.maxLife > 0.0 ? saturate(1.0 - p.life / p.maxLife) : 1.0;
        p.color = lerp(g_colorStart, g_colorEnd, t);
    }
    else
    {
        p.life = -1.0;
    }

    g_particles[i] = p;
}
