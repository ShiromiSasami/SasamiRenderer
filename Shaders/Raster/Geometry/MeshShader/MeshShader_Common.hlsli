#ifndef SASAMI_MESH_SHADER_COMMON_HLSLI
#define SASAMI_MESH_SHADER_COMMON_HLSLI

// GPU layout of one virtual-geometry cluster (ClusterDagGpuBuffer.h GpuClusterDesc, 48 bytes)
struct ClusterDesc
{
    uint   vertexOffset;    // into g_clusterVertices (global vertex ids of the mesh VB)
    uint   vertexCount;     // <= 128
    uint   triangleOffset;  // into g_clusterTriangles (one uint per triangle, 8:8:8 local indices)
    uint   triangleCount;   // <= 128
    uint   group;           // owning group (parent error)  -- absolute index into g_groups
    uint   refinedGroup;    // finer group this cluster was simplified from, or kInvalidIndex (absolute index)
    float3 boundsCenter;    // mesh space
    float  boundsRadius;
    float2 pad;
};

// GPU layout of one DAG group (GpuGroupDesc, 32 bytes)
struct GroupDesc
{
    float3 center;       // merged sphere, mesh space
    float  radius;
    float  error;        // simplification error of the coarser result, mesh units; FLT_MAX-ish for terminal groups
    uint   depth;
    uint   firstCluster; // absolute index into g_clusters of this group's first member cluster
    uint   clusterCount; // number of member clusters
};

// GPU layout of one cluster-DAG BVH node (32 bytes). group == kInvalidIndex marks an
// internal node whose children are the absolute node range [childOffset, childOffset +
// childCount); otherwise the node is a leaf referencing group (absolute index into
// g_groups). error is the conservative max simplification error over the whole subtree,
// so a subtree can be rejected without visiting its children (clodBuildHierarchy rule,
// meshoptimizer clusterlod.h; Karis, "Nanite: A Deep Dive", SIGGRAPH 2021).
struct NodeDesc
{
    float3 center;
    float  radius;
    float  error;
    uint   group;
    uint   childOffset;
    uint   childCount;
};

// GPU layout of one virtual-geometry instance (96 bytes). clusterOffset/clusterCount
// index g_clusters, nodeOffset/rootNodeCount are the absolute range of this instance's
// BVH forest roots in g_nodes ([nodeOffset, nodeOffset + rootNodeCount)).
struct InstanceDesc
{
    row_major float4x4 model;
    uint  clusterOffset;
    uint  clusterCount;
    uint  nodeOffset;
    uint  rootNodeCount;
    uint  vertexSrvIndex;
    uint3 pad;
};

static const uint kInvalidIndex        = 0xFFFFFFFFu;
static const uint kClusterMaxVertices  = 128u;
static const uint kClusterMaxTriangles = 128u;
static const uint kAsGroupSize         = 64u;   // clusters evaluated per amplification group
static const uint kIndirectGroupsX     = 1024u; // mesh-shader indirect DispatchMesh is 2D: x = min(count, kIndirectGroupsX)

// CS_ClusterSelect runs one thread group per instance, and D3D12 caps a Dispatch at 65535 thread
// groups per dimension, so that dispatch is 2D with this width - packed exactly like the indirect
// DispatchMesh above (x = min(count, width), y = ceil(count / width), flat = groupId.y * width +
// groupId.x, surplus groups early-return). Unlike kIndirectGroupsX, whose producer and consumer are
// both HLSL, this width's producer is C++: ClusterGpuCulling.cpp holds a mirror named kSelectGroupsX.
// The two MUST hold the same value - C++ issues the Dispatch, the shader rebuilds the flat instance
// index from this constant, and a divergence silently selects the wrong instances with no error.
static const uint kSelectGroupsX       = 1024u; // cluster-selection Dispatch is 2D: x = min(instanceCount, kSelectGroupsX)

// AS/MS/PS resource bindings below (CameraCB, DrawCB, g_clusters, g_groups, ASPayload).
// ClusterSelect_CS.hlsl has its own, unrelated compute root signature whose cbuffer and
// SRV names/registers would otherwise collide with these; it #defines this macro before
// including this header so it only pulls in the types/constants/functions above and below.
#ifndef SASAMI_MESH_SHADER_COMMON_SKIP_AS_RESOURCES

// b0, visibility ALL. Matches CameraCBData (RendererFrameCoordinator.cpp):
//   mvp = viewProj, world = proj, extra0 = (cameraPos.xyz, lodThresholdPx),
//   extra1 = (projScale, zNear, orthographic, 0)
// For an orthographic view (directional shadow cascade), g_projScale is instead
// texels per world unit (= shadowMapSize / (2 * orthoHalfHeight)).
cbuffer CameraCB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_proj;
    float3 g_cameraPos;
    float  g_lodThresholdPx;   // target screen-space error in pixels
    float  g_projScale;        // proj[1][1] * 0.5 * viewportHeight  (mesh-space error -> pixels at distance 1)
    float  g_zNear;
    float  g_orthographic;   // 1 = orthographic view (directional shadow cascade): screen error is distance-independent
    float  g_clusterDebugMode; // 0 = off, 1 = colour by cluster, 2 = colour by group (LOD set)
    // extra2.x: bindless heap index of the null Texture2D SRV, substituted for a material's
    // kInvalidSrvIndex texture slot by the GPU-driven GBuffer PS (same fallback the CPU path
    // binds through its descriptor tables). Unused by every other mesh-shader stage.
    uint   g_nullTextureSrvIndex;
    float3 g_cameraCbPad;
    // extra3..extra5: SDF face shadow for the GBuffer PSs (ToonFaceShadow.hlsli). The mesh-shader
    // root signatures have no LightCB, so ClusterDrawSubmitter copies the face frame here.
    // Zero (= off) in every other pass, because PushCameraCB clears unused extras.
    float3 g_toonFaceToLight;      // unit vector towards the directional light
    float  g_toonFaceEnabled;      // 1 = LightSystem::ToonFaceSettings::enabled
    float3 g_toonFaceForward;
    float  g_toonFaceSoftness;
    float3 g_toonFaceRight;
    uint   g_toonFaceMapSrvIndex;  // bindless index for the indirect PS; the direct PS uses t9
}

// b1, 36 root constants, visibility ALL
struct DrawCBData
{
    row_major float4x4 model;
    row_major float4x4 inverseModel;
    uint clusterOffset;   // first cluster of this mesh in g_clusters
    uint clusterCount;
    uint groupOffset;     // unused by the shaders (group indices in ClusterDesc are already absolute) — keep for layout
    uint pad;
};
cbuffer DrawCB : register(b1) { DrawCBData g_draw; }

StructuredBuffer<ClusterDesc> g_clusters : register(t0);   // AS + MS
StructuredBuffer<GroupDesc>   g_groups   : register(t7);   // AS

struct ASPayload { uint clusterIndices[kAsGroupSize]; };   // absolute cluster indices

#endif // SASAMI_MESH_SHADER_COMMON_SKIP_AS_RESOURCES

// Streaming page pool layout (ClusterPageStore, C++ side): a page starts 16-byte aligned in
// a ByteAddressBuffer pool. header (16 B) = uint clusterCount, uint vertexIdCount,
// uint triangleCount, uint group. Then ClusterDesc[clusterCount] at page+16 (48 B each,
// vertexOffset/triangleOffset are element indices relative to this page's sections).
// vertexIds (uint, global vertex ids) at align16(16 + clusterCount*48). triangles
// (uint, 8:8:8) at align16(vertexIdsOffset + vertexIdCount*4).
static const uint kPageHeaderBytes   = 16u;
static const uint kClusterDescBytes  = 48u;
static const uint kInvalidPageOffset = 0xFFFFFFFFu;

uint Align16(uint x) { return (x + 15u) & ~15u; }

uint PageVertexIdsOffset(uint clusterCount)
{
    return Align16(kPageHeaderBytes + clusterCount * kClusterDescBytes);
}

uint PageTrianglesOffset(uint clusterCount, uint vertexIdCount)
{
    return Align16(PageVertexIdsOffset(clusterCount) + vertexIdCount * 4u);
}

// Load one ClusterDesc (48 bytes: 6 uints, float3, float, 2 pad) from a page pool at a byte
// address (page start + kPageHeaderBytes + clusterIndexInPage * kClusterDescBytes).
ClusterDesc LoadClusterDesc(ByteAddressBuffer pool, uint byteAddress)
{
    uint4 a = pool.Load4(byteAddress);
    uint4 b = pool.Load4(byteAddress + 16u);
    uint4 c = pool.Load4(byteAddress + 32u);

    ClusterDesc desc;
    desc.vertexOffset   = a.x;
    desc.vertexCount    = a.y;
    desc.triangleOffset = a.z;
    desc.triangleCount  = a.w;
    desc.group          = b.x;
    desc.refinedGroup   = b.y;
    desc.boundsCenter   = asfloat(uint3(b.z, b.w, c.x));
    desc.boundsRadius   = asfloat(c.y);
    desc.pad            = asfloat(c.zw);
    return desc;
}

// Visible-list entry: x = instance index in the low kVisibleInstanceBits, the cluster's index
// within that page's ClusterDesc array in the remaining high bits; y = the page byte offset in
// all 32 bits. Both fields live in x, so their widths trade against each other.
//
// Bit budget, from the measured page layout rather than a guess. Bistro, render.pagestats:
// pages=8023 maxClusters=21 oversizedPages=0 -- the widest page needs 5 bits, because
// ClusterDagBuildSettings::partitionSize targets 16 clusters per group and clusterlod's
// partitioner overshoots that only slightly. 8 bits (255 clusters per page) is therefore ~12x
// the observed maximum, and leaves 24 bits = 16,777,216 instances, so the instance field is no
// longer the first ceiling on the way to 100k procedural instances.
//
// History: the 27/5 split capped a page at 31 clusters and the pool at 2 GB. The 16/16 split
// that replaced it capped instances at 65,536, past which the index spilled into the cluster
// field and drew a cluster with another instance's matrix, with no diagnostic at all.
// Two guards now sit on these widths: ClusterPageStore::kMaxClustersPerPage mirrors
// kVisibleClusterBits and reports any page that would not fit, and ClusterSelect_CS raises the
// kOverflow* flags below when a field or the visible list itself runs out of room at run time.
static const uint kVisibleInstanceBits = 24u;
static const uint kVisibleClusterBits  = 32u - kVisibleInstanceBits;           // 8
static const uint kVisibleInstanceMask = (1u << kVisibleInstanceBits) - 1u;    // 0x00FFFFFF
static const uint kVisibleClusterMask  = (1u << kVisibleClusterBits)  - 1u;    // 0x000000FF
static const uint kVisibleClusterShift = kVisibleInstanceBits;

// Flag bits OR-ed into the visible counter's byte 4 by ClusterSelect_CS and read back as
// ClusterStreamingStats::overflowFlags. Each one means some work was dropped or mispacked, so
// none of them can be reached without a report.
static const uint kOverflowTraversalList = 1u;   // the per-instance BVH node list filled up
static const uint kOverflowInstanceIndex = 2u;   // instance index wider than kVisibleInstanceBits
static const uint kOverflowClusterInPage = 4u;   // page holds more clusters than kVisibleClusterBits can index
static const uint kOverflowVisibleList   = 8u;   // the visible list was full; this cluster was not drawn
// 16u is raised on the CPU, not here: ClusterStreamingFeedback::kOverflowInstanceCapacity, set when
// the cluster instance upload buffer could not be grown to hold every instance of the frame. Listed
// so the numbering stays allocated in one place and the next shader flag does not collide with it.

uint2 PackVisibleEntry(uint instance, uint pageOffset, uint clusterInPage)
{
    // Masked so an out-of-range field (flagged separately) stays inside its own bits instead of
    // corrupting its neighbour: a wrapped instance index draws the wrong instance, but never
    // the wrong cluster of it.
    return uint2((instance & kVisibleInstanceMask) |
                 ((clusterInPage & kVisibleClusterMask) << kVisibleClusterShift), pageOffset);
}

void UnpackVisibleEntry(uint2 e, out uint instance, out uint pageOffset, out uint clusterInPage)
{
    instance      = e.x & kVisibleInstanceMask;
    clusterInPage = (e.x >> kVisibleClusterShift) & kVisibleClusterMask;
    pageOffset    = e.y;
}

// Check if a world-space sphere is inside the view frustum.
// Planes extracted from the combined VP matrix (Gribb-Hartmann).
bool IsSphereVisible(float3 center, float radius, row_major float4x4 vp)
{
    // Row-major access: vp[row][col]
    float4 planes[6];
    // left
    planes[0] = float4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0],
                       vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
    // right
    planes[1] = float4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0],
                       vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
    // bottom
    planes[2] = float4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1],
                       vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
    // top
    planes[3] = float4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1],
                       vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
    // near
    planes[4] = float4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    // far
    planes[5] = float4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2],
                       vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);

    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float len  = length(planes[i].xyz);
        float dist = dot(planes[i].xyz, center) + planes[i].w;
        if (dist < -radius * len)
            return false;
    }
    return true;
}

// Projected screen-space error (pixels) of a world-space bounding sphere with mesh-space
// error 'worldError' already scaled to world units, per Karis 2021 / meshoptimizer
// clusterlod.h: the error is divided by distance to the radius-inflated bounding sphere,
// then converted to pixels via projScale. Terminal groups/subtrees carry a huge/near-
// infinite error so the "coarser than threshold" test is always true for them.
float ScreenErrorPx(float3 worldCenter, float worldRadius, float worldError,
                     float3 cameraPos, float zNear, float projScale, float orthographic)
{
    // Orthographic projection has no perspective divide, so mesh-space error maps to
    // texels by a constant factor (clusterlod.h's formula with distance = 1).
    float d = (orthographic > 0.5f) ? 1.0f : max(distance(worldCenter, cameraPos) - worldRadius, zNear);
    return worldError / d * projScale;
}

#endif
