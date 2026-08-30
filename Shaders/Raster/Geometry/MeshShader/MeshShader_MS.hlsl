// MeshShader_MS.hlsl - Mesh Shader: per-meshlet triangle output
// Reads: t0=MeshletDesc[], t1=VertexBuffer (StructuredBuffer<Vertex>),
//        t2=MeshletIndex buffer, b0=CameraCB, b1=DrawCB
// Outputs: up to 192 vertices (64 tris * 3), up to 64 primitives.

#include "Shared/Common/MeshletConstants.hlsli"

struct MeshletDesc
{
    uint   indexOffset;
    uint   indexCount;
    float3 boundsCenter;
    float  boundsRadius;
};

struct Vertex
{
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
};

struct DrawCBData
{
    row_major float4x4 model;
    row_major float4x4 inverseModel;
    uint     meshletOffset;
    uint     meshletCount;
    uint     pad0;
    uint     pad1;
};

cbuffer CameraCB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_proj;
    float3   g_cameraPos;
    float    g_cameraPad;
}

cbuffer DrawCB : register(b1)
{
    DrawCBData g_draw;
}

StructuredBuffer<MeshletDesc> g_meshletDescs   : register(t0);
StructuredBuffer<Vertex>      g_vertices        : register(t1);
Buffer<uint>                  g_meshletIndices  : register(t2);

struct ASPayload
{
    uint     meshletIndex;
    row_major float4x4 model;
    row_major float4x4 inverseModel;
};

// Must match PBR_PS.hlsl PSInput
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

// Meshlet size = 64 triangles (kMaxTrianglesPerMeshlet in MeshletBuffer.h).
// Each triangle is output as-is; culled meshlets never reach the MS at all
// (AS only calls DispatchMesh(1,1,1,...) for meshlets that pass the cull).
// kMaxTrianglesPerMeshlet comes from Shared/Common/MeshletConstants.hlsli.
static const uint GROUP_SIZE  = kMaxTrianglesPerMeshlet; // one wave, one thread per triangle
static const uint kMaxVerts   = 3u * kMaxTrianglesPerMeshlet; // 192 verts (no vertex sharing)
static const uint kMaxPrims   = kMaxTrianglesPerMeshlet;      // 64 prims

// Per-primitive attribute: carries the exact meshlet index to the PS.
// Currently unread - the MS pipeline pairs with CookTorranceGGX_PS, and the
// meshlet debug view runs on the VS path (see MeshletDebug_PS.hlsl), which cannot
// receive a per-primitive attribute.  Kept because it is the correct source for the
// meshlet index once a mesh-shader debug PSO exists; unconsumed MS outputs are legal.
struct MeshletPrimAttr
{
    uint meshletIdx : MESHLET_INDEX;
};

// Helper: build a PSInput by fetching vertex 'vi' and transforming it.
PSInput MakeVertex(uint vi, row_major float4x4 model, row_major float4x4 invModel)
{
    Vertex vert = g_vertices[vi];

    float4 worldPos4  = mul(float4(vert.position, 1.0f), model);
    float3x3 normalMat = (float3x3)invModel;
    // Matrix-first so the normal gets transpose(inverse(model)); positions above use
    // the row-vector product, so mul(normal, normalMat) would apply the inverse
    // rotation instead and light rotated meshes from the wrong side.
    float3 worldNorm   = normalize(mul(normalMat, vert.normal));

    PSInput o;
    o.position = mul(worldPos4, g_viewProj);
    o.worldPos = worldPos4.xyz;
    o.worldN   = worldNorm;
    o.color    = vert.color;
    o.uv       = vert.uv;
    o.lightPos = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
}

[numthreads(GROUP_SIZE, 1, 1)]
[outputtopology("triangle")]
void MS_Meshlet(
    in  payload ASPayload      inPayload,
    uint        gtid           : SV_GroupThreadID,
    out vertices PSInput       outVerts[kMaxVerts],
    out indices  uint3         outPrims[kMaxPrims],
    out primitives MeshletPrimAttr outPrimAttrs[kMaxPrims])
{
    MeshletDesc desc   = g_meshletDescs[inPayload.meshletIndex];
    uint inputTriCount = min(desc.indexCount, kMaxTrianglesPerMeshlet);

    SetMeshOutputCounts(inputTriCount * 3u, inputTriCount);

    // Strided loop instead of a single `if (gtid < inputTriCount)`: if GROUP_SIZE
    // is ever raised past kMaxTrianglesPerMeshlet this still emits every triangle
    // exactly once instead of silently dropping the ones beyond thread count.
    for (uint t = gtid; t < inputTriCount; t += GROUP_SIZE)
    {
        uint baseIdx = desc.indexOffset + t * 3u;
        uint vBase   = t * 3u;

        outVerts[vBase + 0] = MakeVertex(g_meshletIndices[baseIdx + 0u], inPayload.model, inPayload.inverseModel);
        outVerts[vBase + 1] = MakeVertex(g_meshletIndices[baseIdx + 1u], inPayload.model, inPayload.inverseModel);
        outVerts[vBase + 2] = MakeVertex(g_meshletIndices[baseIdx + 2u], inPayload.model, inPayload.inverseModel);

        outPrims[t] = uint3(vBase, vBase + 1u, vBase + 2u);
        outPrimAttrs[t].meshletIdx = inPayload.meshletIndex;
    }
}
