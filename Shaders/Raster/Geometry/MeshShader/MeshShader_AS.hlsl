// MeshShader_AS.hlsl - Amplification Shader: per-cluster frustum + Nanite-style LOD cull.
//
// LOD selection mirrors meshoptimizer's clusterlod.h and Karis, "Nanite: A Deep Dive"
// (SIGGRAPH 2021): a cluster is rendered iff its own group's coarser simplification is
// too coarse for the screen (projected error above threshold) AND either it is source
// geometry (no finer group to defer to) or its finer group is already fine enough. This
// selects exactly one LOD cut through the cluster DAG for the current view.
//
// Reads: t0=ClusterDesc[], t7=GroupDesc[], b0=CameraCB, b1=DrawCB
// Dispatches one MS group per surviving batch of up to kAsGroupSize clusters.

#include "Raster/Geometry/MeshShader/MeshShader_Common.hlsli"

// Projected screen-space error (pixels) of a DAG group's simplification, per Karis 2021 /
// meshoptimizer clusterlod.h: the mesh-space error and bounding radius are scaled by the
// draw's uniform world scale, then handed to the shared ScreenErrorPx (MeshShader_Common.hlsli).
// Terminal groups carry a huge/near-infinite error so the "coarser than threshold" test is
// always true for source-level clusters.
float ScreenErrorPx(GroupDesc g, float worldScale)
{
    float3 wc = mul(float4(g.center, 1.0f), g_draw.model).xyz;
    float  wr = g.radius * worldScale;
    float  e  = g.error * worldScale;
    return ScreenErrorPx(wc, wr, e, g_cameraPos, g_zNear, g_projScale, g_orthographic);
}

groupshared uint      gs_visibleCount;
groupshared ASPayload gs_payload;

[numthreads(kAsGroupSize, 1, 1)]
void AS_Meshlet(uint dtid : SV_DispatchThreadID, uint gtid : SV_GroupThreadID)
{
    if (gtid == 0u)
    {
        gs_visibleCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (dtid < g_draw.clusterCount)
    {
        uint clusterIndex = g_draw.clusterOffset + dtid;
        ClusterDesc c     = g_clusters[clusterIndex];

        float3 worldCenter = mul(float4(c.boundsCenter, 1.0f), g_draw.model).xyz;

        float sx = length(g_draw.model[0].xyz);
        float sy = length(g_draw.model[1].xyz);
        float sz = length(g_draw.model[2].xyz);
        float worldScale  = max(sx, max(sy, sz));
        float worldRadius = c.boundsRadius * worldScale;

        bool frustumVisible = IsSphereVisible(worldCenter, worldRadius, g_viewProj);

        // Render iff own group's coarser version is too coarse AND (source geometry OR
        // this cluster's finer version is fine enough).
        bool lodSelected =
            ScreenErrorPx(g_groups[c.group], worldScale) > g_lodThresholdPx &&
            (c.refinedGroup == kInvalidIndex ||
             ScreenErrorPx(g_groups[c.refinedGroup], worldScale) <= g_lodThresholdPx);

        bool visible = lodSelected && frustumVisible;

        if (visible)
        {
            uint slot;
            InterlockedAdd(gs_visibleCount, 1u, slot);
            gs_payload.clusterIndices[slot] = clusterIndex;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // D3D12 amplification shaders must call DispatchMesh exactly once on all paths.
    DispatchMesh(gs_visibleCount, 1u, 1u, gs_payload);
}
