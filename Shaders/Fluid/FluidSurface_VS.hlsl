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
    // How disturbed the surface is at this vertex, 0 = glassy. Drives foam in the pixel shader.
    float  agitation   : TEXCOORD2;
};

// Height at an arbitrary position in cell space, clamped at the edges the same way the compute
// shader's SampleHeight() does. The mesh is finer than the grid, so every read has to interpolate:
// sampling the nearest cell instead quantises whatever is derived from it into visible bands.
float SampleHeightBilinear(float fx, float fz)
{
    fx = clamp(fx, 0.0f, (float)(countX - 1u));
    fz = clamp(fz, 0.0f, (float)(countZ - 1u));
    uint x0 = (uint)fx;
    uint z0 = (uint)fz;
    uint x1 = min(x0 + 1u, countX - 1u);
    uint z1 = min(z0 + 1u, countZ - 1u);
    float tx = fx - (float)x0;
    float tz = fz - (float)z0;
    float h00 = g_heightState[z0 * countX + x0].x;
    float h10 = g_heightState[z0 * countX + x1].x;
    float h01 = g_heightState[z1 * countX + x0].x;
    float h11 = g_heightState[z1 * countX + x1].x;
    return lerp(lerp(h00, h10, tx), lerp(h01, h11, tx), tz);
}

// Vertical velocity, interpolated the same way as the height. The state buffer packs
// (height, velocity) per cell -- velocity is the wave equation's integrated force term, so it is
// large exactly where the surface is being pushed around rather than merely displaced.
float SampleVelocityBilinear(float fx, float fz)
{
    fx = clamp(fx, 0.0f, (float)(countX - 1u));
    fz = clamp(fz, 0.0f, (float)(countZ - 1u));
    uint x0 = (uint)fx;
    uint z0 = (uint)fz;
    uint x1 = min(x0 + 1u, countX - 1u);
    uint z1 = min(z0 + 1u, countZ - 1u);
    float tx = fx - (float)x0;
    float tz = fz - (float)z0;
    float v00 = g_heightState[z0 * countX + x0].y;
    float v10 = g_heightState[z0 * countX + x1].y;
    float v01 = g_heightState[z1 * countX + x0].y;
    float v11 = g_heightState[z1 * countX + x1].y;
    return lerp(lerp(v00, v10, tx), lerp(v01, v11, tx), tz);
}

PSInput VSMain(VSInput input)
{
    // gridCoord is normalized [0,1] over the fluid region (see GenerateGridVertices), so the mesh
    // resolution and the simulation grid resolution no longer have to match. Scale into cell space
    // and sample bilinearly: the mesh is usually finer than the grid, and nearest sampling would
    // leave the surface visibly faceted instead of smooth.
    float fx = saturate(input.gridCoord.x) * (float)(countX - 1u);
    float fz = saturate(input.gridCoord.y) * (float)(countZ - 1u);
    float height = SampleHeightBilinear(fx, fz);

    // World position follows the continuous coordinate, not the snapped cell, so the surface is
    // not quantised to the grid.
    float worldX = originX + fx * cellSize;
    float worldY = surfaceHeight + height;
    float worldZ = originZ + fz * cellSize;

    // Central difference taken at the *continuous* coordinate. Reading the snapped cell here was
    // what banded the surface: position and height interpolate smoothly while the normal stayed
    // piecewise constant across each cell, so the Fresnel term (and with it the alpha) stepped in
    // stripes at exactly the grid pitch. Nothing was missing from the mesh -- the pale bands were
    // the water itself turning more transparent.
    float heightLeft  = SampleHeightBilinear(fx - 1.0f, fz);
    float heightRight = SampleHeightBilinear(fx + 1.0f, fz);
    float heightUp    = SampleHeightBilinear(fx, fz - 1.0f);
    float heightDown  = SampleHeightBilinear(fx, fz + 1.0f);

    float dHdx = (heightRight - heightLeft) / (2.0f * cellSize);
    float dHdz = (heightDown - heightUp) / (2.0f * cellSize);
    float3 worldNormal = normalize(float3(-dHdx, 1.0f, -dHdz));

    // Foam forms where the surface is being churned, not merely where it is high. Both signals
    // here are deliberately **dimensionless**, because the first attempt was not and it flooded
    // the whole surface white: it used |laplacian| * invCellSizeSq, which carries units of
    // 1/length^2 and reaches ~8 on a modest ripple at cellSize 0.25, against a threshold picked
    // as if the value were O(1). A threshold can only be a constant if what it compares against
    // is scale-free.
    //
    //   steepness  -- |grad h|, the surface slope, already computed above for the normal. This is
    //                 the classic whitecap criterion: ocean renderers break foam out past a
    //                 steepness/Jacobian threshold rather than proportionally to height.
    //   speedRatio -- |velocity| / wave speed. Water moving at an appreciable fraction of the
    //                 wave speed is agitated even at the rest height, where steepness reads zero.
    //
    // Either one alone leaves a gap: steepness blinks out as a crest flattens, velocity smears
    // foam across the whole swell.
    float steepness = length(float2(dHdx, dHdz));
    float waveSpeed = sqrt(max(waveSpeedSq, 1e-6f));
    float speedRatio = abs(SampleVelocityBilinear(fx, fz)) / waveSpeed;

    PSInput o;
    o.svPosition  = mul(float4(worldX, worldY, worldZ, 1.0f), viewProj);
    o.worldPos    = float3(worldX, worldY, worldZ);
    o.worldNormal = worldNormal;
    o.agitation   = steepness + speedRatio;
    return o;
}
