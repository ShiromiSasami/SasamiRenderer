//
// FluidHeightfield_CS.hlsl
// GPU heightfield / shallow-water ripple simulation using the discrete 2D
// wave equation (Muller & Fischer, GDC 2008, "Fast Water Simulation for
// Games Using Height Fields"). One dispatch advances the (height, velocity)
// state by dt; ping-ponging between the read and write buffers is driven
// from C++.
//
// Bindings (root descriptors):
//   b0 : FluidUpdateCB   (root CBV)
//   t0 : g_readState     (current state, root SRV)
//   u0 : g_writeState    (next state,    root UAV)
//

cbuffer FluidUpdateCB : register(b0)
{
    float    g_originX;
    float    g_originZ;
    float    g_cellSize;
    float    g_invCellSizeSq;
    uint     g_countX;
    uint     g_countZ;
    float    g_waveSpeedSq;
    float    g_damping;
    float    g_dt;
    uint     g_splashActive;
    float    g_splashX;
    float    g_splashZ;
    float    g_splashRadius;
    float    g_splashStrength;
    float    g_surfaceHeight; // unused here; consumed by FluidSurface_VS
    float    g_pad1;
};

StructuredBuffer<float2>   g_readState  : register(t0);
RWStructuredBuffer<float2> g_writeState : register(u0);

float SampleHeight(uint2 coord, StructuredBuffer<float2> buf, uint countX, uint countZ)
{
    uint cx = min(coord.x, countX - 1);
    uint cz = min(coord.y, countZ - 1);
    return buf[cz * countX + cx].x;
}

[numthreads(8, 8, 1)]
void CS_FluidUpdate(uint3 id : SV_DispatchThreadID)
{
    uint x = id.x;
    uint z = id.y;
    if (x >= g_countX || z >= g_countZ)
    {
        return;
    }

    uint idx = z * g_countX + x;

    float2 state    = g_readState[idx];
    float  height   = state.x;
    float  velocity = state.y;

    uint leftX  = (x == 0)              ? 0            : x - 1;
    uint rightX = (x + 1 >= g_countX)   ? g_countX - 1  : x + 1;
    uint upZ    = (z == 0)              ? 0            : z - 1;
    uint downZ  = (z + 1 >= g_countZ)   ? g_countZ - 1  : z + 1;

    float left  = SampleHeight(uint2(leftX, z), g_readState, g_countX, g_countZ);
    float right = SampleHeight(uint2(rightX, z), g_readState, g_countX, g_countZ);
    float up    = SampleHeight(uint2(x, upZ), g_readState, g_countX, g_countZ);
    float down  = SampleHeight(uint2(x, downZ), g_readState, g_countX, g_countZ);

    float sumNeighbors = left + right + up + down;

    // Discrete 2D wave equation (Muller & Fischer, GDC 2008).
    float force = g_waveSpeedSq * (sumNeighbors - 4.0f * height) * g_invCellSizeSq;
    velocity += force * g_dt;
    velocity *= g_damping;
    height += velocity * g_dt;

    if (g_splashActive != 0)
    {
        float worldX = g_originX + x * g_cellSize;
        float worldZ = g_originZ + z * g_cellSize;
        float dist   = length(float2(worldX - g_splashX, worldZ - g_splashZ));
        if (dist < g_splashRadius)
        {
            height += g_splashStrength * saturate(1.0f - dist / g_splashRadius);
        }
    }

    g_writeState[idx] = float2(height, velocity);
}
