// MeshShaderShadowIndirect_MS.hlsl - Mesh Shader: depth-only cluster output for the
// directional shadow cascades, driven by ClusterSelect_CS.hlsl's GPU-culled visible list
// instead of an amplification-shader payload. Dispatched with one indirect DispatchMesh
// call (CS_ClusterSelectArgs); there is no pixel shader. g_viewProj holds the cascade's
// light view-projection. Per-instance vertex buffers are fetched through the bindless
// descriptor heap (InstanceDesc.vertexSrvIndex), since each instance may reference a
// different mesh's vertex buffer.
// Reads: t0=page pool (ClusterPageStore, ByteAddressBuffer), t1=InstanceDesc[],
//        t2=visible list, t3=visible counter, b0=CameraCB (cascade light VP, from Common)
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

struct ShadowVertex
{
    float4 position : SV_POSITION;
};

// Build a ShadowVertex from vertex 'vi' of instance 'inst''s bindless vertex buffer,
// transformed with the instance's model matrix and the cascade's light view-projection.
ShadowVertex MakeVertex(InstanceDesc inst, uint vi)
{
    StructuredBuffer<Vertex> vb = ResourceDescriptorHeap[inst.vertexSrvIndex];
    Vertex vert = vb[vi];

    ShadowVertex o;
    o.position = mul(mul(float4(vert.position, 1.0f), inst.model), g_viewProj);
    return o;
}

[numthreads(kClusterMaxTriangles, 1, 1)]
[outputtopology("triangle")]
void MS_ShadowIndirect(
    uint         gtid : SV_GroupThreadID,
    uint3        gid  : SV_GroupID,
    out vertices ShadowVertex outVerts[kClusterMaxVertices],
    out indices  uint3        outPrims[kClusterMaxTriangles])
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

    if (gtid < c.vertexCount)
    {
        uint vid = g_pagePool.Load(vertexIdsBase + (c.vertexOffset + gtid) * 4u);
        outVerts[gtid] = MakeVertex(inst, vid);
    }

    if (gtid < c.triangleCount)
    {
        uint packed = g_pagePool.Load(trianglesBase + (c.triangleOffset + gtid) * 4u);
        outPrims[gtid] = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
    }
}
