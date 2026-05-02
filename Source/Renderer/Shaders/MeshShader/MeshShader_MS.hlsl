// MeshShader_MS.hlsl - Mesh Shader: per-meshlet triangle output
// Reads: t0=MeshletDesc[], t1=VertexBuffer (StructuredBuffer<Vertex>),
//        t2=MeshletIndex buffer, b0=CameraCB, b1=DrawCB
// Outputs: up to 192 vertices (64 tris * 3), up to 64 primitives.

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
    uint     lodLevel;      // 0=full, 1=half, 2=quarter triangle count
    uint     pad0;
    uint     pad1;
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

// Meshlet size = 16 triangles (kMaxTrianglesPerMeshlet in MeshletBuffer.h).
// LOD 0: each triangle → 4 sub-triangles (midpoint subdivision) → 16×4 = 64 tris, 16×12 = 192 verts
// LOD 1: each triangle → 1 triangle (output as-is)              → 16 tris,  16×3  =  48 verts
// LOD 2: meshlet is culled entirely by AS (MS never invoked)
static const uint kMaxVerts = 192u; // 16 tris × 12 verts (LOD-0 subdivided, no vertex sharing)
static const uint kMaxPrims =  64u; // 16 tris × 4 sub-tris

// Per-primitive attribute: carries the exact meshlet index to the PS.
// This allows the debug PS to color each meshlet independently of LOD level
// (SV_PrimitiveID / N breaks at LOD 0 because 64 output prims ≠ 16 input tris).
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
    float3 worldNorm   = normalize(mul(vert.normal, normalMat));

    PSInput o;
    o.position = mul(worldPos4, g_viewProj);
    o.worldPos = worldPos4.xyz;
    o.worldN   = worldNorm;
    o.color    = vert.color;
    o.uv       = vert.uv;
    o.lightPos = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
}

// Linearly interpolate two PSInput values (position, worldPos, normal, uv, color).
PSInput LerpVertex(PSInput a, PSInput b, float t)
{
    PSInput o;
    o.position = lerp(a.position, b.position, t);
    o.worldPos = lerp(a.worldPos, b.worldPos, t);
    o.worldN   = normalize(lerp(a.worldN,   b.worldN,   t));
    o.color    = lerp(a.color,   b.color,   t);
    o.uv       = lerp(a.uv,      b.uv,      t);
    o.lightPos = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
}

[numthreads(16, 1, 1)]
[outputtopology("triangle")]
void MS_Meshlet(
    in  payload ASPayload      inPayload,
    uint        gtid           : SV_GroupThreadID,
    out vertices PSInput       outVerts[kMaxVerts],
    out indices  uint3         outPrims[kMaxPrims],
    out primitives MeshletPrimAttr outPrimAttrs[kMaxPrims])
{
    MeshletDesc desc   = g_meshletDescs[inPayload.meshletIndex];
    uint inputTriCount = min(desc.indexCount, 16u);
    uint lod           = inPayload.lodLevel; // 0 or 1 (LOD2 is culled by AS)

    if (lod == 0u)
    {
        // LOD 0 — midpoint subdivision: 1 input tri → 4 output tris, 12 verts (no sharing)
        SetMeshOutputCounts(inputTriCount * 12u, inputTriCount * 4u);

        if (gtid < inputTriCount)
        {
            uint baseIdx = desc.indexOffset + gtid * 3u;
            uint vi0 = g_meshletIndices[baseIdx + 0u];
            uint vi1 = g_meshletIndices[baseIdx + 1u];
            uint vi2 = g_meshletIndices[baseIdx + 2u];

            // Corner vertices
            PSInput v0 = MakeVertex(vi0, inPayload.model, inPayload.inverseModel);
            PSInput v1 = MakeVertex(vi1, inPayload.model, inPayload.inverseModel);
            PSInput v2 = MakeVertex(vi2, inPayload.model, inPayload.inverseModel);

            // Edge midpoints
            PSInput m01 = LerpVertex(v0, v1, 0.5f);
            PSInput m12 = LerpVertex(v1, v2, 0.5f);
            PSInput m20 = LerpVertex(v2, v0, 0.5f);

            // Write 12 vertices (no sharing between input-triangle groups)
            uint vBase = gtid * 12u;
            outVerts[vBase +  0] = v0;   outVerts[vBase +  1] = m01; outVerts[vBase +  2] = m20;
            outVerts[vBase +  3] = m01;  outVerts[vBase +  4] = v1;  outVerts[vBase +  5] = m12;
            outVerts[vBase +  6] = m20;  outVerts[vBase +  7] = m12; outVerts[vBase +  8] = v2;
            outVerts[vBase +  9] = m01;  outVerts[vBase + 10] = m12; outVerts[vBase + 11] = m20;

            // Write 4 primitives + per-primitive meshlet index
            uint pBase = gtid * 4u;
            outPrims[pBase + 0] = uint3(vBase +  0, vBase +  1, vBase +  2);
            outPrims[pBase + 1] = uint3(vBase +  3, vBase +  4, vBase +  5);
            outPrims[pBase + 2] = uint3(vBase +  6, vBase +  7, vBase +  8);
            outPrims[pBase + 3] = uint3(vBase +  9, vBase + 10, vBase + 11);
            outPrimAttrs[pBase + 0].meshletIdx = inPayload.meshletIndex;
            outPrimAttrs[pBase + 1].meshletIdx = inPayload.meshletIndex;
            outPrimAttrs[pBase + 2].meshletIdx = inPayload.meshletIndex;
            outPrimAttrs[pBase + 3].meshletIdx = inPayload.meshletIndex;
        }
    }
    else
    {
        // LOD 1 — output triangles as-is (matches HS TessFactor=2 path)
        SetMeshOutputCounts(inputTriCount * 3u, inputTriCount);

        if (gtid < inputTriCount)
        {
            uint baseIdx = desc.indexOffset + gtid * 3u;
            uint vBase   = gtid * 3u;

            outVerts[vBase + 0] = MakeVertex(g_meshletIndices[baseIdx + 0u], inPayload.model, inPayload.inverseModel);
            outVerts[vBase + 1] = MakeVertex(g_meshletIndices[baseIdx + 1u], inPayload.model, inPayload.inverseModel);
            outVerts[vBase + 2] = MakeVertex(g_meshletIndices[baseIdx + 2u], inPayload.model, inPayload.inverseModel);

            outPrims[gtid] = uint3(vBase, vBase + 1u, vBase + 2u);
            outPrimAttrs[gtid].meshletIdx = inPayload.meshletIndex;
        }
    }
}
