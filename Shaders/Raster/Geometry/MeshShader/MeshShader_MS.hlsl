// MeshShader_MS.hlsl - Mesh Shader: per-cluster triangle output for virtual-geometry clusters.
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

// Must match MeshShaderGBuffer_PS.hlsl PSInput
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
    nointerpolation uint2 clusterInfo : TEXCOORD3; // x = absolute cluster index, y = owning group
};

// Build a PSInput from vertex 'vi', transformed with the current draw's model matrix
// (DrawCB, b1 - visible to the mesh shader stage).
PSInput MakeVertex(uint vi)
{
    Vertex vert = g_vertices[vi];

    float4 worldPos4    = mul(float4(vert.position, 1.0f), g_draw.model);
    float3x3 normalMat  = (float3x3)g_draw.inverseModel;
    // Matrix-first so the normal gets transpose(inverse(model)); positions above use
    // the row-vector product on the other side.
    float3 worldNorm     = normalize(mul(normalMat, vert.normal));

    PSInput o;
    o.position = mul(worldPos4, g_viewProj);
    o.worldPos = worldPos4.xyz;
    o.worldN   = worldNorm;
    o.color    = vert.color;
    o.uv       = vert.uv;
    o.lightPos = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
}

[numthreads(kClusterMaxTriangles, 1, 1)]
[outputtopology("triangle")]
void MS_Meshlet(
    uint         gtid : SV_GroupThreadID,
    uint         gid  : SV_GroupID,
    in  payload  ASPayload inPayload,
    out vertices PSInput   outVerts[kClusterMaxVertices],
    out indices  uint3     outPrims[kClusterMaxTriangles])
{
    ClusterDesc c = g_clusters[inPayload.clusterIndices[gid]];

    SetMeshOutputCounts(c.vertexCount, c.triangleCount);

    if (gtid < c.vertexCount)
    {
        outVerts[gtid] = MakeVertex(g_clusterVertices[c.vertexOffset + gtid]);
        outVerts[gtid].clusterInfo = uint2(inPayload.clusterIndices[gid], c.group);
    }

    if (gtid < c.triangleCount)
    {
        uint packed = g_clusterTriangles[c.triangleOffset + gtid];
        outPrims[gtid] = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
    }
}
