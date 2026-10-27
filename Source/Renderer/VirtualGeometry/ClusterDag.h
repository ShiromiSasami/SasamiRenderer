#pragma once

// Cluster LOD hierarchy (DAG) of one static mesh: the CPU-side data model for the
// virtual-geometry pipeline. Produced by ClusterDagBuilder (meshoptimizer clusterlod.h),
// persisted by ClusterDagSerializer, owned per mesh by ClusterDagStore.
//
// Data layout and LOD-selection semantics follow clusterlod.h (meshoptimizer 1.2) and
// Karis, "Nanite: A Deep Dive", SIGGRAPH 2021 Advances in Real-Time Rendering:
//   * a cluster is rendered iff  groups[cluster.group].simplified.error  is over the
//     screen-space threshold and (cluster.refinedGroup == kInvalidIndex or
//     groups[cluster.refinedGroup].simplified.error is at or under it);
//   * group.simplified.error is monotonic up the DAG (parent >= child), which makes that
//     test crack-free between neighbouring groups.

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    // Mesh-space bounding sphere plus the accumulated simplification error (mesh units).
    struct ClusterDagBounds
    {
        float center[3] = { 0.0f, 0.0f, 0.0f };
        float radius = 0.0f;
        float error = 0.0f;
    };

    // One meshlet-sized cluster (<= ClusterDagBuildSettings::maxTriangles triangles).
    // Indices address the source Mesh's vertex array: simplification never creates new
    // vertices, so every LOD level shares the original vertex buffer.
    struct ClusterDagCluster
    {
        uint32_t indexOffset = 0;   // first index in ClusterDag::indices
        uint32_t indexCount = 0;    // multiple of 3
        uint32_t vertexCount = 0;   // unique vertices referenced by this cluster
        uint32_t group = 0;         // owning group; its `simplified` is this cluster's parent error
        uint32_t refinedGroup = 0;  // finer group whose simplification produced this cluster;
                                    // kInvalidIndex for source (LOD 0) geometry
        ClusterDagBounds bounds;    // culling sphere. bounds.error is NOT monotonic across the
                                    // DAG; use the group errors for LOD selection.
    };

    // A set of adjacent clusters that were simplified together into the next-coarser level.
    struct ClusterDagGroup
    {
        uint32_t firstCluster = 0;  // clusters[firstCluster .. firstCluster + clusterCount)
        uint32_t clusterCount = 0;
        uint32_t depth = 0;         // DAG level of the member clusters (0 = source geometry)
        ClusterDagBounds simplified; // merged sphere of the members and the error of the
                                     // coarser result. error == FLT_MAX for terminal groups
                                     // (the final root, or a group that could not simplify)
    };

    // Node of the per-level BVH forest used to accelerate runtime cluster selection.
    struct ClusterDagNode
    {
        ClusterDagBounds bounds;    // conservative over the whole subtree
        uint32_t group = 0;         // leaf: index into groups; internal: kInvalidIndex
        uint32_t childOffset = 0;   // internal: first child node index
        uint32_t childCount = 0;    // internal: number of children (<= kHierarchyNodeWidth)
    };

    struct ClusterDag
    {
        static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

        std::vector<uint32_t>          indices;   // all clusters' triangle lists, concatenated
        std::vector<ClusterDagCluster> clusters;
        std::vector<ClusterDagGroup>   groups;    // generation order == increasing depth
        std::vector<ClusterDagNode>    nodes;     // forest: nodes[0 .. levelCount) are the roots,
                                                  // one per DAG depth

        uint32_t levelCount = 0;          // number of DAG depths (root groups)
        uint32_t terminalGroupCount = 0;  // groups with simplified.error == FLT_MAX
        uint32_t sourceTriangleCount = 0; // triangles of the input mesh
        uint32_t sourceVertexCount = 0;   // vertices of the input mesh (indices are < this)

        bool IsEmpty() const { return clusters.empty(); }
    };

    // Build parameters. Part of the cache key: changing any field invalidates cached DAGs.
    struct ClusterDagBuildSettings
    {
        uint32_t maxTriangles = 128;   // triangles per cluster (Nanite uses 128)
        uint32_t maxVertices = 128;    // vertices per cluster
        uint32_t partitionSize = 16;   // target clusters per group (clusterlod default)
        float    simplifyRatio = 0.5f; // per-level triangle reduction target
        uint32_t hierarchyNodeWidth = 8; // children per BVH node
    };
}

namespace SasamiEngine { using namespace Render; }
