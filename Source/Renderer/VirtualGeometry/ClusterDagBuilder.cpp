// Offline cluster-DAG build on top of meshoptimizer's clusterlod.h (v1.2). Design sources:
// clusterlod.h (meshoptimizer 1.2) and Karis, "Nanite: A Deep Dive", SIGGRAPH 2021 Advances in
// Real-Time Rendering.
//
// This is the only translation unit that defines CLUSTERLOD_IMPLEMENTATION: clusterlod.h is a
// single-header library and its implementation must appear exactly once in the link.

#include "Renderer/VirtualGeometry/ClusterDagBuilder.h"
#include "Renderer/Structures/Mesh.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cstring>
#include <vector>

// clusterlod.h's implementation section relies on assert(), std::max, memcpy etc. arriving
// transitively; the standard headers above must precede it.
#include "meshoptimizer/meshoptimizer.h"
#define CLUSTERLOD_IMPLEMENTATION
#include "meshoptimizer/clusterlod.h"

namespace SasamiEngine::Render
{
    namespace
    {
        void CopyBounds(ClusterDagBounds& dst, const clodBounds& src)
        {
            dst.center[0] = src.center[0];
            dst.center[1] = src.center[1];
            dst.center[2] = src.center[2];
            dst.radius = src.radius;
            dst.error = src.error;
        }

        // Functor passed to the C++ overload of clodBuild: called once per emitted group, with
        // the group's clusters. Appends both to the DAG and returns the group index, which
        // clodBuild records as clodCluster::refined on the clusters produced from this group's
        // simplification.
        struct GroupCollector
        {
            ClusterDag& dag;
            std::vector<clodGroup>& groups;

            int operator()(clodGroup group, const clodCluster* clusters, size_t clusterCount)
            {
                const uint32_t groupIndex = static_cast<uint32_t>(dag.groups.size());

                ClusterDagGroup dagGroup;
                dagGroup.firstCluster = static_cast<uint32_t>(dag.clusters.size());
                dagGroup.clusterCount = static_cast<uint32_t>(clusterCount);
                dagGroup.depth = static_cast<uint32_t>(group.depth);
                CopyBounds(dagGroup.simplified, group.simplified);
                dag.groups.push_back(dagGroup);
                groups.push_back(group);

                for (size_t i = 0; i < clusterCount; ++i)
                {
                    const clodCluster& cluster = clusters[i];

                    ClusterDagCluster dagCluster;
                    dagCluster.indexOffset = static_cast<uint32_t>(dag.indices.size());
                    dagCluster.indexCount = static_cast<uint32_t>(cluster.index_count);
                    dagCluster.vertexCount = static_cast<uint32_t>(cluster.vertex_count);
                    dagCluster.group = groupIndex;
                    dagCluster.refinedGroup = cluster.refined < 0
                        ? ClusterDag::kInvalidIndex
                        : static_cast<uint32_t>(cluster.refined);
                    CopyBounds(dagCluster.bounds, cluster.bounds);

                    // cluster.indices is only valid for the lifetime of this callback.
                    dag.indices.insert(dag.indices.end(), cluster.indices, cluster.indices + cluster.index_count);
                    dag.clusters.push_back(dagCluster);
                }

                return static_cast<int>(groupIndex);
            }
        };
    }

    ClusterDag ClusterDagBuilder::Build(const Mesh& mesh, const ClusterDagBuildSettings& settings)
    {
        ClusterDag dag;

        const uint32_t vertexCount = mesh.VertexCount();
        const bool hasIndices = mesh.HasIndices();
        const size_t indexCount = hasIndices ? mesh.indices.size() : size_t(vertexCount);

        dag.sourceVertexCount = vertexCount;
        dag.sourceTriangleCount = static_cast<uint32_t>(indexCount / 3);

        if (indexCount == 0 || indexCount % 3 != 0 || vertexCount == 0)
        {
            return dag;
        }

        std::vector<uint32_t> sequentialIndices;
        const uint32_t* indices = nullptr;
        if (hasIndices)
        {
            indices = mesh.indices.data();
        }
        else
        {
            sequentialIndices.resize(indexCount);
            for (size_t i = 0; i < indexCount; ++i)
            {
                sequentialIndices[i] = static_cast<uint32_t>(i);
            }
            indices = sequentialIndices.data();
        }

        for (size_t i = 0; i < indexCount; ++i)
        {
            if (indices[i] >= vertexCount)
            {
                return dag;
            }
        }

        // Packed {normal.xyz, uv.xy} per vertex. Weights and the UV-only protect mask are the
        // meshoptimizer clusterlod demo defaults and are tunables.
        std::vector<float> attributes(size_t(vertexCount) * 5);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            const Vertex& vertex = mesh.vertices[i];
            float* attribute = &attributes[size_t(i) * 5];
            attribute[0] = vertex.normal[0];
            attribute[1] = vertex.normal[1];
            attribute[2] = vertex.normal[2];
            attribute[3] = vertex.uv[0];
            attribute[4] = vertex.uv[1];
        }

        const float attributeWeights[5] = { 0.5f, 0.5f, 0.5f, 1.0f, 1.0f };

        clodMesh clodMeshDesc = {};
        clodMeshDesc.indices = indices;
        clodMeshDesc.index_count = indexCount;
        clodMeshDesc.vertex_count = vertexCount;
        clodMeshDesc.vertex_positions = &mesh.vertices[0].position[0];
        clodMeshDesc.vertex_positions_stride = sizeof(Vertex);
        clodMeshDesc.vertex_attributes = attributes.data();
        clodMeshDesc.vertex_attributes_stride = 5 * sizeof(float);
        clodMeshDesc.vertex_lock = nullptr;
        clodMeshDesc.attribute_weights = attributeWeights;
        clodMeshDesc.attribute_count = 5;
        clodMeshDesc.attribute_protect_mask = (1u << 3) | (1u << 4);

        clodConfig config = clodDefaultConfig(settings.maxTriangles);
        config.max_vertices = settings.maxVertices;
        config.partition_size = settings.partitionSize;
        config.simplify_ratio = settings.simplifyRatio;

        std::vector<clodGroup> groups;
        GroupCollector collector{ dag, groups };
        clodBuild(config, clodMeshDesc, collector);

        uint32_t levelCount = 0;
        uint32_t terminalGroupCount = 0;
        for (const clodGroup& group : groups)
        {
            levelCount = std::max(levelCount, static_cast<uint32_t>(group.depth) + 1);
            if (group.simplified.error == FLT_MAX)
            {
                ++terminalGroupCount;
            }
        }
        dag.levelCount = levelCount;
        dag.terminalGroupCount = terminalGroupCount;

        if (!groups.empty())
        {
            const size_t nodeBound = clodBuildHierarchyBound(groups.size(), settings.hierarchyNodeWidth, levelCount);
            std::vector<clodNode> nodes(nodeBound);
            const size_t nodeCount = clodBuildHierarchy(nodes.data(), groups.data(), groups.size(),
                                                         settings.hierarchyNodeWidth, levelCount);

            dag.nodes.resize(nodeCount);
            for (size_t i = 0; i < nodeCount; ++i)
            {
                const clodNode& node = nodes[i];
                ClusterDagNode& dagNode = dag.nodes[i];
                CopyBounds(dagNode.bounds, node.bounds);
                dagNode.group = node.group < 0 ? ClusterDag::kInvalidIndex : static_cast<uint32_t>(node.group);
                dagNode.childOffset = node.child_offset;
                dagNode.childCount = node.child_count;
            }
        }

        return dag;
    }
}
