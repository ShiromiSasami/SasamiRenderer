// MeshShaderGBufferIndirect_MS.hlsl - Mesh Shader: GBuffer cluster output for the main view,
// driven by ClusterSelect_CS.hlsl's GPU-culled visible list instead of an amplification-shader
// payload. Dispatched with one indirect DispatchMesh call (CS_ClusterSelectArgs). Per-instance
// vertex buffers are fetched through the bindless descriptor heap (InstanceDesc.vertexSrvIndex);
// the instance index is forwarded to the pixel shader so it can fetch its material.
// Reads: t0=page pool (ClusterPageStore, ByteAddressBuffer), t1=InstanceDesc[],
//        t2=visible list, t3=visible counter, b0=CameraCB (main view VP, from Common)
// Outputs: up to kClusterMaxVertices vertices, up to kClusterMaxTriangles primitives.

#include "Raster/Geometry/MeshShader/MeshShader_Common.hlsli"

struct Vertex
{
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
};

ByteAddressBuffer              g_pagePool  : register(t0);
StructuredBuffer<InstanceDesc> g_instances : register(t1);
StructuredBuffer<uint2>        g_visible   : register(t2);
ByteAddressBuffer              g_counter   : register(t3);

// Must match MeshShaderGBufferIndirect_PS.hlsl PSInput
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    // Cluster provenance for the debug views (g_clusterDebugMode). nointerpolation because
    // these are integers and constant across the whole cluster.
    nointerpolation uint2 clusterInfo : TEXCOORD3; // x = page offset + cluster in page, y = owning group
    nointerpolation uint instanceIndex : TEXCOORD4;
};

// Build a PSInput from vertex 'vi' of instance 'inst''s bindless vertex buffer. 'cofactor'
// holds the three per-instance cross products used to transform normals (see MS entry).
PSInput MakeVertex(InstanceDesc inst, uint vi, float3 cofactor[3], float detSign)
{
    StructuredBuffer<Vertex> vb = ResourceDescriptorHeap[inst.vertexSrvIndex];
    Vertex vert = vb[vi];

    float4 worldPos4 = mul(float4(vert.position, 1.0f), inst.model);
    // Cofactor matrix of the upper 3x3 = det * inverse^T, so it transforms normals exactly.
    // This matches MeshShader_MS.hlsl's mul((float3x3)inverseModel, n) = n * inverse^T up to
    // a positive scale (normalize removes it; detSign keeps mirrored instances oriented).
    float3 worldNorm = normalize(detSign * (vert.normal.x * cofactor[0]
                                          + vert.normal.y * cofactor[1]
                                          + vert.normal.z * cofactor[2]));

    PSInput o;
    o.position = mul(worldPos4, g_viewProj);
    o.worldPos = worldPos4.xyz;
    o.worldN   = worldNorm;
    o.color    = vert.color;
    o.uv       = vert.uv;
    o.lightPos = float4(0.0f, 0.0f, 0.0f, 1.0f);
    o.clusterInfo   = uint2(0u, 0u);
    o.instanceIndex = 0u;
    return o;
}

[numthreads(kClusterMaxTriangles, 1, 1)]
[outputtopology("triangle")]
void MS_GBufferIndirect(
    uint         gtid : SV_GroupThreadID,
    uint3        gid  : SV_GroupID,
    out vertices PSInput outVerts[kClusterMaxVertices],
    out indices  uint3   outPrims[kClusterMaxTriangles])
{
    uint flatIndex = gid.y * kIndirectGroupsX + gid.x;
    uint count     = g_counter.Load(0);

    // DXC validation allows exactly one SetMeshOutputCounts call per shader, so the
    // out-of-range groups (the dispatch is padded to a multiple of kIndirectGroupsX)
    // emit zero outputs through the same call instead of an early-return branch.
    const bool valid = flatIndex < count;
    uint2 v = valid ? g_visible[flatIndex] : uint2(0, 0);

    uint instanceIndex, pageOffset, clusterInPage;
    UnpackVisibleEntry(v, instanceIndex, pageOffset, clusterInPage);

    InstanceDesc inst = g_instances[instanceIndex];
    ClusterDesc  c    = LoadClusterDesc(g_pagePool, pageOffset + kPageHeaderBytes + clusterInPage * kClusterDescBytes);
    const uint vertexCount   = valid ? c.vertexCount   : 0u;
    const uint triangleCount = valid ? c.triangleCount : 0u;

    SetMeshOutputCounts(vertexCount, triangleCount);
    if (!valid)
    {
        return;
    }

    uint4 hdr = g_pagePool.Load4(pageOffset);
    uint vertexIdsBase = pageOffset + PageVertexIdsOffset(hdr.x);
    uint trianglesBase = pageOffset + PageTrianglesOffset(hdr.x, hdr.y);

    // Per-instance normal transform (row-vector convention, p' = p * M). The model matrix
    // carries no inverse, so use the cofactor of the upper 3x3 computed once per thread.
    float3 r0 = inst.model[0].xyz;
    float3 r1 = inst.model[1].xyz;
    float3 r2 = inst.model[2].xyz;
    float3 cofactor[3] = { cross(r1, r2), cross(r2, r0), cross(r0, r1) };
    float  detSign     = (dot(r0, cofactor[0]) < 0.0f) ? -1.0f : 1.0f;

    if (gtid < c.vertexCount)
    {
        uint vid = g_pagePool.Load(vertexIdsBase + (c.vertexOffset + gtid) * 4u);
        PSInput o = MakeVertex(inst, vid, cofactor, detSign);
        // The absolute cluster index is not in the visible entry; page offset + index in page
        // is unique per resident cluster, which is all the debug hash needs.
        o.clusterInfo   = uint2(pageOffset + clusterInPage, c.group);
        o.instanceIndex = instanceIndex;
        outVerts[gtid] = o;
    }

    if (gtid < c.triangleCount)
    {
        uint packed = g_pagePool.Load(trianglesBase + (c.triangleOffset + gtid) * 4u);
        outPrims[gtid] = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
    }
}
