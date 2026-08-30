//
// FluidSurface_VS.hlsl
// Displaces a flat NxN grid mesh by the GPU heightfield simulation state
// (Muller & Fischer, GDC 2008) written by FluidHeightfield_CS.hlsl, and
// derives a per-vertex surface normal via central differences.
//
// Bindings (root descriptors):
//   b0 : CameraCB          (root CBV, ALL visibility)
//   b1 : FluidGridInfoCB   (root CBV, ALL visibility) - subset view of FluidUpdateCBData
//   t0 : g_heightState     (current (height, velocity) state, root SRV, VERTEX only)
//

cbuffer CameraCB : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 world;
    float4             extra0;
    float4             extra1;
    float4             extra2;
};

// Mirrors FluidHeightfieldSim::FluidUpdateCBData byte-for-byte; only a subset
// of the fields is used here.
cbuffer FluidGridInfoCB : register(b1)
{
    float    originX;
    float    originZ;
    float    cellSize;
    float    invCellSizeSq;
    uint     countX;
    uint     countZ;
    float    waveSpeedSq;
    float    damping;
    float    dt;
    uint     splashActive;
    float    splashX;
    float    splashZ;
    float    splashRadius;
    float    splashStrength;
    float    surfaceHeight;   // world-space Y of the rest surface
    float    pad1;
};

StructuredBuffer<float2> g_heightState : register(t0);

struct VSInput
{
    float2 gridCoord : POSITION0;
};

struct PSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    uint gx = clamp((uint)input.gridCoord.x, 0u, countX - 1u);
    uint gz = clamp((uint)input.gridCoord.y, 0u, countZ - 1u);

    uint idx    = gz * countX + gx;
    float height = g_heightState[idx].x;

    float worldX = originX + gx * cellSize;
    float worldY = surfaceHeight + height;
    float worldZ = originZ + gz * cellSize;

    // Central-difference surface normal from neighboring heights, clamped at
    // grid edges the same way as the compute shader's SampleHeight().
    uint leftX  = (gx == 0)            ? 0u          : gx - 1u;
    uint rightX = (gx + 1u >= countX)  ? countX - 1u : gx + 1u;
    uint upZ    = (gz == 0)            ? 0u          : gz - 1u;
    uint downZ  = (gz + 1u >= countZ)  ? countZ - 1u : gz + 1u;

    float heightLeft  = g_heightState[gz * countX + leftX].x;
    float heightRight = g_heightState[gz * countX + rightX].x;
    float heightUp    = g_heightState[upZ * countX + gx].x;
    float heightDown  = g_heightState[downZ * countX + gx].x;

    float dHdx = (heightRight - heightLeft) / (2.0f * cellSize);
    float dHdz = (heightDown - heightUp) / (2.0f * cellSize);
    float3 worldNormal = normalize(float3(-dHdx, 1.0f, -dHdz));

    PSInput o;
    o.svPosition  = mul(float4(worldX, worldY, worldZ, 1.0f), viewProj);
    o.worldPos    = float3(worldX, worldY, worldZ);
    o.worldNormal = worldNormal;
    return o;
}
