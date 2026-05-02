cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;   // object -> clip (used for screen-space LOD)
    row_major float4x4 u_world; // not used here
}

#include "Common/LightCB.hlsli"

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
    float3 patchDebugColor : COLOR1; // unique color per patch for debug visualization
};

// 16-colour debug palette (matches MeshletDebug_PS.hlsl for visual consistency)
static const float3 kPatchDebugColors[16] =
{
    float3(0.94f, 0.20f, 0.20f),
    float3(0.20f, 0.75f, 0.95f),
    float3(0.20f, 0.85f, 0.30f),
    float3(0.95f, 0.80f, 0.10f),
    float3(0.85f, 0.20f, 0.90f),
    float3(0.10f, 0.80f, 0.70f),
    float3(0.95f, 0.50f, 0.10f),
    float3(0.40f, 0.20f, 0.95f),
    float3(0.95f, 0.55f, 0.75f),
    float3(0.30f, 0.95f, 0.60f),
    float3(0.70f, 0.40f, 0.10f),
    float3(0.60f, 0.95f, 0.20f),
    float3(0.10f, 0.40f, 0.90f),
    float3(0.95f, 0.30f, 0.55f),
    float3(0.50f, 0.95f, 0.90f),
    float3(0.75f, 0.20f, 0.40f),
};

// Distance-based LOD thresholds — must match MeshShader_AS.hlsl exactly.
//   LOD 0 (< 5 m)   : TessFactor = 8  (subdivide 3 times → ×4 triangles)
//   LOD 1 (5–15 m)  : TessFactor = 2  (minimal subdivision)
//   LOD 2 (≥ 15 m)  : TessFactor = 0  (cull patch — same as mesh shader culling meshlet)
static const float kLodNear = 5.0f;
static const float kLodFar  = 15.0f;

// Compute a tessellation factor from the clip-space depth of one edge midpoint.
// clip.w equals view-space Z for a standard perspective projection.
float EdgeFactor(float3 objMid)
{
    float4 c     = mul(float4(objMid, 1.0f), u_mvp);
    float depth  = max(c.w, 0.001f);
    if (depth <  kLodNear) return 8.0f;  // LOD 0 – dense
    if (depth <  kLodFar)  return 2.0f;  // LOD 1 – medium
    return 0.0f;                          // LOD 2 – cull
}

// Returns true when all three control points project outside the NDC frustum
// (with a 10 % margin to avoid popping at exact frustum boundary).
bool IsPatchOffscreen(InputPatch<HSInput, 3> patch)
{
    // Use a conservative threshold: all three must be outside the *same* plane.
    float4 c0 = mul(float4(patch[0].position, 1.0f), u_mvp);
    float4 c1 = mul(float4(patch[1].position, 1.0f), u_mvp);
    float4 c2 = mul(float4(patch[2].position, 1.0f), u_mvp);

    // Behind the near plane?
    if (c0.w <= 0.0f && c1.w <= 0.0f && c2.w <= 0.0f)
        return true;

    const float kMargin = 1.1f;
    float3 x = float3(c0.x / max(c0.w, 0.001f),
                      c1.x / max(c1.w, 0.001f),
                      c2.x / max(c2.w, 0.001f));
    float3 y = float3(c0.y / max(c0.w, 0.001f),
                      c1.y / max(c1.w, 0.001f),
                      c2.y / max(c2.w, 0.001f));

    if (all(x < -kMargin) || all(x > kMargin)) return true;
    if (all(y < -kMargin) || all(y > kMargin)) return true;

    return false;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstants")]
HSInput HSMain(InputPatch<HSInput, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}

HSConst PatchConstants(InputPatch<HSInput, 3> patch, uint patchId : SV_PrimitiveID)
{
    HSConst pc;

    // Cull off-screen patches entirely (hardware skips tessellation when factor <= 0).
    if (IsPatchOffscreen(patch))
    {
        pc.edges[0] = pc.edges[1] = pc.edges[2] = 0.0f;
        pc.inside   = 0.0f;
        return pc;
    }

    // Per-edge factor based on the depth of the edge midpoint.
    // DX12 convention: edges[i] is the edge *opposite* vertex i.
    float3 mid01 = (patch[0].position + patch[1].position) * 0.5f;
    float3 mid12 = (patch[1].position + patch[2].position) * 0.5f;
    float3 mid20 = (patch[2].position + patch[0].position) * 0.5f;

    pc.edges[0] = EdgeFactor(mid12); // opposite vertex 0
    pc.edges[1] = EdgeFactor(mid20); // opposite vertex 1
    pc.edges[2] = EdgeFactor(mid01); // opposite vertex 2
    pc.inside   = (pc.edges[0] + pc.edges[1] + pc.edges[2]) / 3.0f;

    // Per-patch debug color: hash patchId into one of 16 palette entries.
    // Uses a simple Murmur-style integer hash for good avalanche behaviour.
    uint h = patchId * 2654435761u;
    h = (h ^ (h >> 13u)) * 1540483477u;
    h =  h ^ (h >> 15u);
    pc.patchDebugColor = kPatchDebugColors[(h >> 8u) & 0xFu];

    return pc;
}
