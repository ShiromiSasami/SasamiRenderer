// ClusterSelect_CS.hlsl - Compute Shader: GPU-driven visibility for virtual geometry.
//
// Traverses each instance's cluster-DAG BVH forest (one thread group per instance) and
// appends the surviving clusters (absolute cluster index, per instance) to g_visible, for
// consumption by a depth-only mesh shader through one indirect DispatchMesh call built by
// CS_ClusterSelectArgs below. The BVH descent and per-node/per-cluster LOD rule mirror
// meshoptimizer's clusterlod.h (clodBuildHierarchy traversal: a subtree can be rejected
// without visiting its children once its conservative max error is below threshold) and
// Karis, "Nanite: A Deep Dive" (SIGGRAPH 2021): a cluster is rendered iff its own group's
// coarser simplification is too coarse for the screen AND either it is source geometry or
// its finer group is already fine enough.
//
// Reads: t0=ClusterDesc[] (unused; resident cluster data now comes from the page pool),
//        t1=GroupDesc[], t2=NodeDesc[], t3=InstanceDesc[], t4=per-group page offsets,
//        t5=page pool, b0=ClusterSelectCB
// Writes: u0=visible list, u1=counters/overflow flag, u2=indirect DispatchMesh args, u3=wanted group bitset

#define SASAMI_MESH_SHADER_COMMON_SKIP_AS_RESOURCES
#include "Raster/Geometry/MeshShader/MeshShader_Common.hlsli"

cbuffer ClusterSelectCB : register(b0)   // root CBV, 128 bytes
{
    row_major float4x4 g_viewProj;
    float3 g_cameraPos;  float g_lodThresholdPx;
    float  g_projScale;  float g_zNear;  float g_orthographic;  uint g_instanceCount;
    uint   g_maxVisible; uint  g_collectStats; uint2 g_selectPad;  // g_collectStats: 0 skips the diagnostic counters below
}

StructuredBuffer<ClusterDesc>  g_clusters  : register(t0);  // unused: resident cluster data is read from g_pagePool instead
StructuredBuffer<GroupDesc>    g_groups    : register(t1);
StructuredBuffer<NodeDesc>     g_nodes     : register(t2);
StructuredBuffer<InstanceDesc> g_instances : register(t3);
ByteAddressBuffer g_groupPageOffset : register(t4);  // uint per ABSOLUTE group: byte offset of its page in g_pagePool, kInvalidPageOffset when not resident
ByteAddressBuffer g_pagePool        : register(t5);  // ClusterPageStore page pool (streaming)

bool IsGroupResident(uint group) { return g_groupPageOffset.Load(group * 4u) != kInvalidPageOffset; }

RWStructuredBuffer<uint2> g_visible : register(u0);  // PackVisibleEntry(instanceIndex, pageOffset, clusterIndexInPage)
RWByteAddressBuffer       g_counter : register(u1);  // byte 0: visible count, byte 4: overflow flag, byte 8: instances processed
RWByteAddressBuffer       g_args    : register(u2);  // 12 bytes: DispatchMesh x,y,z
RWByteAddressBuffer       g_groupWanted : register(u3);  // bitset over ABSOLUTE group indices: bit set = a selected cluster belongs to this group (streaming request set)

static const uint kBvhListCapacity = 512u;
static const uint kBvhGroupSize    = 64u;

groupshared uint gs_listA[kBvhListCapacity];
groupshared uint gs_listB[kBvhListCapacity];
groupshared uint gs_countA;
groupshared uint gs_countB;
groupshared uint gs_currentIsA;
// Diagnostics only (counter bytes 12/16/20), gated by g_collectStats: how much traversal this
// dispatch actually did. Added 2026-09-17 to test whether the shadow pass's ~4.25ms selection
// floor is the BVH walk. It is not: examined varies 47x (30613 -> 644) with no change in timing,
// so the floor is per-view fixed cost, not this shader's work. Their own overhead measured below
// run-to-run noise, but they stay opt-in so the steady-state path runs no extra atomics.
groupshared uint gs_nodesVisited;
groupshared uint gs_levels;
groupshared uint gs_clustersExamined;

// Screen-space error (pixels) of a world-space sphere with mesh-space error/radius already
// scaled by the instance's uniform world scale. Shared with the per-cluster group test below.
float WorldSphereErrorPx(float3 meshCenter, float meshRadius, float meshError,
                          row_major float4x4 model, float worldScale)
{
    float3 worldCenter = mul(float4(meshCenter, 1.0f), model).xyz;
    float  worldRadius = meshRadius * worldScale;
    float  worldError  = meshError * worldScale;
    return ScreenErrorPx(worldCenter, worldRadius, worldError, g_cameraPos, g_zNear, g_projScale, g_orthographic);
}

// Append 'value' to the groupshared list selected by 'toA' (true -> gs_listA/gs_countA,
// false -> gs_listB/gs_countB). Sets the overflow flag and drops the value if full.
void PushNode(bool toA, uint value)
{
    uint slot;
    if (toA)
    {
        InterlockedAdd(gs_countA, 1u, slot);
        if (slot < kBvhListCapacity)
        {
            gs_listA[slot] = value;
        }
        else
        {
            g_counter.InterlockedOr(4, kOverflowTraversalList);
        }
    }
    else
    {
        InterlockedAdd(gs_countB, 1u, slot);
        if (slot < kBvhListCapacity)
        {
            gs_listB[slot] = value;
        }
        else
        {
            g_counter.InterlockedOr(4, kOverflowTraversalList);
        }
    }
}

[numthreads(kBvhGroupSize, 1, 1)]
void CS_ClusterSelect(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    // 2D dispatch (kSelectGroupsX in MeshShader_Common.hlsli): one thread group per instance would
    // blow past D3D12's 65535 groups-per-dimension in 1D, so C++ dispatches x = min(instanceCount,
    // kSelectGroupsX), y = ceil(instanceCount / kSelectGroupsX) and the flat instance index is
    // recovered here - the same packing MeshShaderShadowIndirect_MS uses for its cluster index.
    uint instanceIndex = groupId.y * kSelectGroupsX + groupId.x;
    // The round-up above hands us up to kSelectGroupsX - 1 surplus groups; they exit here.
    if (instanceIndex >= g_instanceCount)
        return;

    uint tid = groupThreadId.x;

    // One flag per group instead of one per packed entry: the index is group-uniform, so this
    // costs a compare on the healthy path and an atomic only on a scene the entry cannot address.
    if (tid == 0u && instanceIndex > kVisibleInstanceMask)
    {
        g_counter.InterlockedOr(4, kOverflowInstanceIndex);
    }

    InstanceDesc inst = g_instances[instanceIndex];

    float sx = length(inst.model[0].xyz);
    float sy = length(inst.model[1].xyz);
    float sz = length(inst.model[2].xyz);
    float worldScale = max(sx, max(sy, sz));

    if (tid == 0u)
    {
        gs_countA       = 0u;
        gs_countB       = 0u;
        gs_currentIsA   = 1u;
        gs_nodesVisited = 0u;
        gs_levels       = 0u;
        gs_clustersExamined = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint localNodesVisited = 0u;   // per-thread tally, folded into gs_nodesVisited once at the end
    uint localClustersExamined = 0u;

    // Seed the current list with this instance's BVH forest roots (absolute node indices).
    for (uint r = tid; r < inst.rootNodeCount; r += kBvhGroupSize)
    {
        PushNode(true, inst.nodeOffset + r);
    }
    GroupMemoryBarrierWithGroupSync();

    [loop]
    for (;;)
    {
        bool currentIsA = (gs_currentIsA != 0u);
        uint currentCount = currentIsA ? gs_countA : gs_countB;
        if (currentCount == 0u)
            break;

        if (tid == 0u)
        {
            if (currentIsA) gs_countB = 0u; else gs_countA = 0u;
            gs_levels += 1u;
        }
        GroupMemoryBarrierWithGroupSync();

        for (uint i = tid; i < currentCount; i += kBvhGroupSize)
        {
            uint nodeIndex = currentIsA ? gs_listA[i] : gs_listB[i];
            NodeDesc node  = g_nodes[nodeIndex];
            ++localNodesVisited;

            float3 worldCenter = mul(float4(node.center, 1.0f), inst.model).xyz;
            float  worldRadius = node.radius * worldScale;

            bool frustumVisible = IsSphereVisible(worldCenter, worldRadius, g_viewProj);
            bool subtreeCoarse  = WorldSphereErrorPx(node.center, node.radius, node.error, inst.model, worldScale) > g_lodThresholdPx;

            if (frustumVisible && subtreeCoarse)
            {
                if (node.group == kInvalidIndex)
                {
                    // Internal node: descend into its children (absolute node indices).
                    for (uint c = 0u; c < node.childCount; ++c)
                    {
                        PushNode(!currentIsA, node.childOffset + c);
                    }
                }
                else
                {
                    // Leaf: evaluate this DAG group's member clusters for the LOD cut. The
                    // group's cluster data lives in a streaming page; a group with no
                    // resident page has nothing to test per-cluster, but is still requested
                    // if it would otherwise be selected, so the streaming system loads it.
                    GroupDesc grp = g_groups[node.group];
                    uint pageOffset = g_groupPageOffset.Load(node.group * 4u);
                    if (pageOffset == kInvalidPageOffset)
                    {
                        bool groupCoarse = WorldSphereErrorPx(grp.center, grp.radius, grp.error,
                                                               inst.model, worldScale) > g_lodThresholdPx;
                        if (groupCoarse)
                        {
                            g_groupWanted.InterlockedOr((node.group >> 5) * 4u, 1u << (node.group & 31u));
                        }
                    }
                    else
                    {
                        // Checked once per leaf rather than per selected cluster: a page this wide
                        // cannot be addressed by the entry's cluster field whichever of its
                        // clusters the LOD cut happens to pick.
                        if (grp.clusterCount > kVisibleClusterMask + 1u)
                        {
                            g_counter.InterlockedOr(4, kOverflowClusterInPage);
                        }

                        for (uint ci = 0u; ci < grp.clusterCount; ++ci)
                        {
                            ClusterDesc c = LoadClusterDesc(g_pagePool, pageOffset + kPageHeaderBytes + ci * kClusterDescBytes);
                            ++localClustersExamined;

                            GroupDesc ownGroup = g_groups[c.group];
                            bool ownCoarse = WorldSphereErrorPx(ownGroup.center, ownGroup.radius, ownGroup.error,
                                                                 inst.model, worldScale) > g_lodThresholdPx;
                            bool ownResident = IsGroupResident(c.group);

                            bool refinedFine = (c.refinedGroup == kInvalidIndex);
                            if (!refinedFine)
                            {
                                // A finer level that is not loaded counts as "fine enough" so the
                                // loaded coarser cluster is drawn in its place — the residency
                                // fallback of vk_lod_clusters / Nanite (Karis 2021, streaming):
                                // no holes, just a temporarily coarser LOD.
                                GroupDesc refinedGroup = g_groups[c.refinedGroup];
                                refinedFine = !IsGroupResident(c.refinedGroup) ||
                                              WorldSphereErrorPx(refinedGroup.center, refinedGroup.radius, refinedGroup.error,
                                                                  inst.model, worldScale) <= g_lodThresholdPx;
                            }

                            float3 clusterWorldCenter = mul(float4(c.boundsCenter, 1.0f), inst.model).xyz;
                            float  clusterWorldRadius = c.boundsRadius * worldScale;
                            bool clusterVisible = IsSphereVisible(clusterWorldCenter, clusterWorldRadius, g_viewProj);

                            if (ownCoarse && refinedFine && clusterVisible)
                            {
                                // Non-resident groups are still requested here so the streaming
                                // system knows to load them, independent of whether this cluster
                                // can be drawn this frame.
                                g_groupWanted.InterlockedOr((c.group >> 5) * 4u, 1u << (c.group & 31u));

                                if (ownResident)
                                {
                                    uint slot;
                                    g_counter.InterlockedAdd(0, 1u, slot);
                                    if (slot < g_maxVisible)
                                    {
                                        g_visible[slot] = PackVisibleEntry(instanceIndex, pageOffset, ci);
                                    }
                                    else
                                    {
                                        // The list is full and this cluster will not be drawn.
                                        // CS_ClusterSelectArgs clamps the count afterwards, so
                                        // without this flag the drop leaves no trace at all.
                                        g_counter.InterlockedOr(4, kOverflowVisibleList);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
        if (tid == 0u)
        {
            gs_currentIsA = currentIsA ? 0u : 1u;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // g_collectStats comes from the constant buffer, so this branch is uniform across the group
    // and the barrier below is still reached by every thread.
    if (g_collectStats != 0u)
    {
        InterlockedAdd(gs_nodesVisited, localNodesVisited);
        InterlockedAdd(gs_clustersExamined, localClustersExamined);
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid == 0u)
    {
        uint dummy;
        g_counter.InterlockedAdd(8, 1u, dummy);
        if (g_collectStats != 0u)
        {
            g_counter.InterlockedAdd(12, gs_nodesVisited, dummy);      // BVH nodes visited
            g_counter.InterlockedMax(16, gs_levels);                   // deepest level count of any instance
            g_counter.InterlockedAdd(20, gs_clustersExamined, dummy);  // cluster descs loaded from the page pool
        }
    }
}

[numthreads(1, 1, 1)]
void CS_ClusterSelectArgs()
{
    // The atomic at byte 0 counts every append the traversal attempted, including the ones that
    // found no slot, so it is the view's actual demand. Clamping it in place used to destroy that
    // number: the readback could then only say "the list was full" (kOverflowVisibleList), never
    // by how much. It is preserved at byte 24 first, and ClusterGpuCulling sizes this view's
    // visible list from it on the next recording of this slot.
    uint requested = g_counter.Load(0);
    uint count = min(requested, g_maxVisible);
    uint x = min(count, kIndirectGroupsX);
    uint y = (count + kIndirectGroupsX - 1u) / kIndirectGroupsX;

    g_counter.Store(24, requested);
    g_counter.Store(0, count);
    g_args.Store3(0, uint3(x, max(y, 1u), 1u));
}
