// DebugProbeGrid_VS.hlsl
// Renders instanced unit-sphere geometry at each irradiance probe position.
//
// Root signature (DebugProbeGrid):
//   [0] b0  : DebugProbeCB (viewProj + probeRadius) — ALL visibility
//   [1] b2  : GIProbeGridCB (origin, spacing, counts) — ALL visibility
//   [2] t10 : Probe SH data — PIXEL visibility

cbuffer DebugProbeCB : register(b0)
{
    row_major float4x4 g_viewProj;      // Camera view * projection
    row_major float4x4 g_worldUnused;   // Unused (kept for PushCameraCB layout compat)
    float4             g_extra0;        // .x = probeRadius
    float4             g_extra1;        // unused
    float4             g_extra2;        // unused
}

// Inline declaration of GIProbeGridCB only — do NOT include GI_Common.hlsli here
// because it also declares g_probeSHData (t10) which is Pixel-only in the root
// signature.  With -Od (debug build) DXC keeps unused resource entries in DXIL
// metadata, causing D3D12 PSO validation to reject the pipeline.
cbuffer GIProbeGridCB : register(b2)
{
    float3 g_probeOrigin;
    float  g_giIntensity;
    float3 g_probeSpacing;
    float  g_giEnabled;
    uint   g_probeCountX;
    uint   g_probeCountY;
    uint   g_probeCountZ;
    uint   g_probeTotalCount;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
};

struct PSInput
{
    float4 svPosition              : SV_POSITION;
    float3 worldN                  : NORMAL;
    nointerpolation uint probeIdx  : TEXCOORD0;
};

PSInput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    // Decode 3D probe coordinates from linear instance index.
    // Guard against division by zero if the CB hasn't been populated yet.
    uint safeX = max(g_probeCountX, 1u);
    uint safeY = max(g_probeCountY, 1u);
    uint idx = instanceId;
    uint x   = idx % safeX;
    uint y   = (idx / safeX) % safeY;
    uint z   = idx / (safeX * safeY);

    float3 probePos = g_probeOrigin + float3((float)x, (float)y, (float)z) * g_probeSpacing;
    float  radius   = max(g_extra0.x, 0.01f);

    float3 worldPos = input.position * radius + probePos;

    PSInput o;
    o.svPosition = mul(float4(worldPos, 1.0f), g_viewProj);
    o.worldN     = input.normal;
    o.probeIdx   = instanceId;
    return o;
}
