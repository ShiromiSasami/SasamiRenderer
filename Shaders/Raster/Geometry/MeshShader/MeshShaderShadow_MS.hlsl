// MeshShaderShadow_MS.hlsl - Mesh Shader: depth-only cluster output for the directional
// shadow cascades. Pairs with MeshShader_AS.hlsl (same payload) in the mesh-shader shadow
// PSO (RenderPipelineStateCacheMeshShader.cpp); there is no pixel shader. g_viewProj holds
// the cascade's light view-projection.
// Reads: t0=ClusterDesc[] (indices from AS payload), t1=VertexBuffer, t2=cluster-local vertex
//        index remap, t6=packed cluster triangles, b0=CameraCB, b1=DrawCB
// Outputs: up to kClusterMaxVertices vertices, up to kClusterMaxTriangles primitives.

#include "Raster/Geometry/MeshShader/MeshShader_Common.hlsli"

struct Vertex
{
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
};

StructuredBuffer<Vertex> g_vertices         : register(t1);
// StructuredBuffer, not a typed Buffer<uint>: t2/t6 are bound as inline root descriptor SRVs
// (RenderPipelineStateCacheMeshShader.cpp) and root SRVs may only be raw or structured buffers.
StructuredBuffer<uint>   g_clusterVertices  : register(t2);
StructuredBuffer<uint>   g_clusterTriangles : register(t6);

struct ShadowVertex
{
    float4 position : SV_POSITION;
};

// Build a ShadowVertex from vertex 'vi', transformed with the current draw's model matrix
// (DrawCB, b1 - visible to the mesh shader stage) and the cascade's light view-projection.
ShadowVertex MakeVertex(uint vi)
{
    Vertex vert = g_vertices[vi];

    ShadowVertex o;
    o.position = mul(mul(float4(vert.position, 1.0f), g_draw.model), g_viewProj);
    return o;
}

[numthreads(kClusterMaxTriangles, 1, 1)]
[outputtopology("triangle")]
void MS_ShadowCluster(
    uint         gtid : SV_GroupThreadID,
    uint         gid  : SV_GroupID,
    in  payload  ASPayload   inPayload,
    out vertices ShadowVertex outVerts[kClusterMaxVertices],
    out indices  uint3        outPrims[kClusterMaxTriangles])
{
    ClusterDesc c = g_clusters[inPayload.clusterIndices[gid]];

    SetMeshOutputCounts(c.vertexCount, c.triangleCount);

    if (gtid < c.vertexCount)
    {
        outVerts[gtid] = MakeVertex(g_clusterVertices[c.vertexOffset + gtid]);
    }

    if (gtid < c.triangleCount)
    {
        uint packed = g_clusterTriangles[c.triangleOffset + gtid];
        outPrims[gtid] = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
    }
}
