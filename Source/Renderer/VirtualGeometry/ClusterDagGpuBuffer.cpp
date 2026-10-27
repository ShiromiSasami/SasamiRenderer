#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"

#include "Renderer/Structures/Mesh.h"
#include "Renderer/VirtualGeometry/ClusterDagStore.h"
#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace SasamiEngine::Render
{
    ClusterDagGpuBuffer::~ClusterDagGpuBuffer()
    {
        Release();
    }

    void ClusterDagGpuBuffer::Release()
    {
        if (m_device)
        {
            if (m_clusterDescBufferHandle.IsValid())
            {
                m_device->DestroyRhiResource(m_clusterDescBufferHandle);
            }
            if (m_groupDescBufferHandle.IsValid())
            {
                m_device->DestroyRhiResource(m_groupDescBufferHandle);
            }
            if (m_clusterVertexBufferHandle.IsValid())
            {
                m_device->DestroyRhiResource(m_clusterVertexBufferHandle);
            }
            if (m_clusterTriangleBufferHandle.IsValid())
            {
                m_device->DestroyRhiResource(m_clusterTriangleBufferHandle);
            }
            if (m_nodeDescBufferHandle.IsValid())
            {
                m_device->DestroyRhiResource(m_nodeDescBufferHandle);
            }
        }

        m_clusterDescBuffer.Reset();
        m_groupDescBuffer.Reset();
        m_clusterVertexBuffer.Reset();
        m_clusterTriangleBuffer.Reset();
        m_nodeDescBuffer.Reset();
        m_clusterDescBufferHandle = {};
        m_groupDescBufferHandle = {};
        m_clusterVertexBufferHandle = {};
        m_clusterTriangleBufferHandle = {};
        m_nodeDescBufferHandle = {};
        m_clusterDescBufferCompat = nullptr;
        m_groupDescBufferCompat = nullptr;
        m_clusterVertexBufferCompat = nullptr;
        m_clusterTriangleBufferCompat = nullptr;
        m_nodeDescBufferCompat = nullptr;
    }

    Resource* ClusterDagGpuBuffer::GetClusterDescBufferResource() const
    {
        return m_clusterDescBufferCompat ? m_clusterDescBufferCompat : const_cast<Resource*>(&m_clusterDescBuffer);
    }

    Resource* ClusterDagGpuBuffer::GetGroupDescBufferResource() const
    {
        return m_groupDescBufferCompat ? m_groupDescBufferCompat : const_cast<Resource*>(&m_groupDescBuffer);
    }

    Resource* ClusterDagGpuBuffer::GetClusterVertexBufferResource() const
    {
        return m_clusterVertexBufferCompat ? m_clusterVertexBufferCompat : const_cast<Resource*>(&m_clusterVertexBuffer);
    }

    Resource* ClusterDagGpuBuffer::GetClusterTriangleBufferResource() const
    {
        return m_clusterTriangleBufferCompat ? m_clusterTriangleBufferCompat : const_cast<Resource*>(&m_clusterTriangleBuffer);
    }

    Resource* ClusterDagGpuBuffer::GetNodeDescBufferResource() const
    {
        return m_nodeDescBufferCompat ? m_nodeDescBufferCompat : const_cast<Resource*>(&m_nodeDescBuffer);
    }

    bool ClusterDagGpuBuffer::IsValid() const
    {
        Resource* clusterDesc = GetClusterDescBufferResource();
        Resource* groupDesc = GetGroupDescBufferResource();
        Resource* clusterVertex = GetClusterVertexBufferResource();
        Resource* clusterTriangle = GetClusterTriangleBufferResource();
        Resource* nodeDesc = GetNodeDescBufferResource();
        return clusterDesc && clusterDesc->IsValid() &&
               groupDesc && groupDesc->IsValid() &&
               clusterVertex && clusterVertex->IsValid() &&
               clusterTriangle && clusterTriangle->IsValid() &&
               nodeDesc && nodeDesc->IsValid();
    }

    std::vector<uint32_t> ClusterDagGpuBuffer::BuildTerminalGroupBits() const
    {
        const uint32_t totalGroups = GetTotalGroupCount();
        std::vector<uint32_t> bits((totalGroups + 31u) / 32u, 0u);
        // Half of FLT_MAX rather than equality: Build writes the sentinel through the same float
        // arithmetic as any other error, so a comparison against the exact value is not safe.
        constexpr float kTerminalErrorThreshold = std::numeric_limits<float>::max() * 0.5f;
        for (uint32_t i = 0; i < totalGroups; ++i) {
            if (m_groups[i].error >= kTerminalErrorThreshold) {
                bits[i / 32u] |= (1u << (i % 32u));
            }
        }
        return bits;
    }

    void ClusterDagGpuBuffer::Build(const std::vector<Mesh>& meshes, const ClusterDagStore& dagStore)
    {
        m_clusters.clear();
        m_groups.clear();
        m_clusterVertices.clear();
        m_clusterTriangles.clear();
        m_nodes.clear();
        m_meshRanges.clear();
        m_meshRanges.reserve(meshes.size());
        m_skippedClusters = 0;

        // Global-vertex -> cluster-local id remap, sized to the largest mesh and reused for every
        // cluster (only the touched entries are reset afterwards). O(1) per lookup with no hashing:
        // a per-cluster hash map took ~12 s for Bistro's 65k clusters in Debug builds.
        size_t maxVertexCount = 0;
        for (const Mesh& mesh : meshes)
        {
            maxVertexCount = (std::max)(maxVertexCount, mesh.vertices.size());
        }
        std::vector<uint32_t> remap(maxVertexCount, ClusterDag::kInvalidIndex);
        std::vector<uint32_t> uniqueVerts;
        std::vector<uint32_t> localTris;
        uniqueVerts.reserve(kMaxVerticesPerCluster);
        localTris.reserve(kMaxTrianglesPerCluster);

        for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
        {
            const Mesh& mesh = meshes[meshIndex];
            const ClusterDag* dag = dagStore.GetDag(meshIndex);

            MeshRange range{};
            range.groupOffset = static_cast<uint32_t>(m_groups.size());
            range.clusterOffset = static_cast<uint32_t>(m_clusters.size());
            range.nodeOffset = static_cast<uint32_t>(m_nodes.size());

            if (!dag || dag->IsEmpty())
            {
                range.groupCount = 0;
                range.clusterCount = 0;
                range.nodeCount = 0;
                range.rootNodeCount = 0;
                m_meshRanges.push_back(range);
                continue;
            }

            for (const ClusterDagGroup& group : dag->groups)
            {
                GpuGroupDesc gd{};
                gd.center[0] = group.simplified.center[0];
                gd.center[1] = group.simplified.center[1];
                gd.center[2] = group.simplified.center[2];
                gd.radius = group.simplified.radius;
                gd.error = group.simplified.error;
                gd.depth = group.depth;
                gd.firstCluster = range.clusterOffset + group.firstCluster;
                gd.clusterCount = group.clusterCount;
                m_groups.push_back(gd);
            }
            range.groupCount = static_cast<uint32_t>(m_groups.size()) - range.groupOffset;

            const uint32_t meshVertexCount = static_cast<uint32_t>(mesh.vertices.size());

            for (const ClusterDagCluster& cluster : dag->clusters)
            {
                GpuClusterDesc desc{};
                desc.vertexOffset = static_cast<uint32_t>(m_clusterVertices.size());
                desc.triangleOffset = static_cast<uint32_t>(m_clusterTriangles.size());
                desc.group = range.groupOffset + cluster.group;
                desc.refinedGroup = (cluster.refinedGroup == ClusterDag::kInvalidIndex)
                                         ? ClusterDag::kInvalidIndex
                                         : range.groupOffset + cluster.refinedGroup;
                desc.boundsCenter[0] = cluster.bounds.center[0];
                desc.boundsCenter[1] = cluster.bounds.center[1];
                desc.boundsCenter[2] = cluster.bounds.center[2];
                desc.boundsRadius = cluster.bounds.radius;
                desc.pad[0] = 0.0f;
                desc.pad[1] = 0.0f;

                // Clusters that fail to fit the mesh-shader limits are still pushed (with
                // triangleCount/vertexCount == 0) so absolute cluster indices stay 1:1 with the
                // DAG's cluster indices; group.firstCluster/clusterCount depend on that.
                const uint32_t triCount = cluster.indexCount / 3u;
                if (triCount > kMaxTrianglesPerCluster)
                {
                    ++m_skippedClusters;
                    desc.vertexCount = 0;
                    desc.triangleCount = 0;
                    m_clusters.push_back(desc);
                    continue;
                }

                uniqueVerts.clear();
                localTris.clear();

                bool ok = true;
                for (uint32_t t = 0; t < triCount && ok; ++t)
                {
                    uint32_t localIds[3];
                    for (uint32_t v = 0; v < 3u; ++v)
                    {
                        const uint32_t vi = dag->indices[cluster.indexOffset + t * 3u + v];
                        if (vi >= meshVertexCount)
                        {
                            ok = false;
                            break;
                        }

                        uint32_t localId = remap[vi];
                        if (localId == ClusterDag::kInvalidIndex)
                        {
                            if (uniqueVerts.size() >= kMaxVerticesPerCluster)
                            {
                                ok = false;
                                break;
                            }
                            localId = static_cast<uint32_t>(uniqueVerts.size());
                            remap[vi] = localId;
                            uniqueVerts.push_back(vi);
                        }
                        localIds[v] = localId;
                    }
                    if (!ok) break;
                    localTris.push_back(static_cast<uint32_t>(localIds[0]) |
                                         (static_cast<uint32_t>(localIds[1]) << 8) |
                                         (static_cast<uint32_t>(localIds[2]) << 16));
                }

                // Reset only the entries this cluster touched so the remap stays valid for the next one.
                for (const uint32_t vi : uniqueVerts)
                {
                    remap[vi] = ClusterDag::kInvalidIndex;
                }

                if (!ok)
                {
                    ++m_skippedClusters;
                    desc.vertexCount = 0;
                    desc.triangleCount = 0;
                    m_clusters.push_back(desc);
                    continue;
                }

                desc.vertexCount = static_cast<uint32_t>(uniqueVerts.size());
                desc.triangleCount = triCount;

                m_clusterVertices.insert(m_clusterVertices.end(), uniqueVerts.begin(), uniqueVerts.end());
                m_clusterTriangles.insert(m_clusterTriangles.end(), localTris.begin(), localTris.end());
                m_clusters.push_back(desc);
            }
            range.clusterCount = static_cast<uint32_t>(m_clusters.size()) - range.clusterOffset;

            for (const ClusterDagNode& node : dag->nodes)
            {
                GpuNodeDesc nd{};
                nd.center[0] = node.bounds.center[0];
                nd.center[1] = node.bounds.center[1];
                nd.center[2] = node.bounds.center[2];
                nd.radius = node.bounds.radius;
                nd.error = node.bounds.error;
                nd.group = (node.group == ClusterDag::kInvalidIndex)
                               ? ClusterDag::kInvalidIndex
                               : range.groupOffset + node.group;
                nd.childOffset = range.nodeOffset + node.childOffset;
                nd.childCount = node.childCount;
                m_nodes.push_back(nd);
            }
            range.nodeCount = static_cast<uint32_t>(m_nodes.size()) - range.nodeOffset;
            range.rootNodeCount = dag->levelCount;

            m_meshRanges.push_back(range);
        }

        if (m_skippedClusters > 0)
        {
            char buf[160];
            snprintf(buf, sizeof(buf),
                "[ClusterDagGpuBuffer] skipped %u cluster(s) exceeding %u vertices / %u triangles.\n",
                m_skippedClusters, kMaxVerticesPerCluster, kMaxTrianglesPerCluster);
            DebugLog(buf);
        }
    }

    bool ClusterDagGpuBuffer::Upload(IRHIDevice& device)
    {
        Release();
        m_device = &device;

        if (m_clusters.empty() && m_groups.empty() && m_clusterVertices.empty() && m_clusterTriangles.empty() &&
            m_nodes.empty())
        {
            return true;
        }

        if (!device.GetCapabilities().supportsRhiResourceCreation)
        {
            return false;
        }

        auto createBuffer = [&](const void* data, size_t elementCount, uint32_t strideInBytes,
                                 RhiBufferHandle& outHandle, Resource*& outCompat) -> bool
        {
            if (elementCount == 0) return true;

            RhiBufferDesc desc{};
            desc.sizeInBytes = static_cast<uint64_t>(elementCount) * strideInBytes;
            desc.strideInBytes = strideInBytes;
            desc.usage = RhiBufferUsageFlags::Structured | RhiBufferUsageFlags::ShaderResource;
            desc.memoryUsage = RhiMemoryUsage::CpuToGpu;
            desc.initialState = RhiResourceState::ShaderResource;
            outHandle = device.CreateRhiBuffer(desc, data);
            outCompat = device.GetD3D12CompatibilityResource(outHandle);
            return outCompat && outCompat->IsValid();
        };

        bool ok = true;
        ok = createBuffer(m_clusters.data(), m_clusters.size(), sizeof(GpuClusterDesc),
                           m_clusterDescBufferHandle, m_clusterDescBufferCompat) && ok;
        ok = createBuffer(m_groups.data(), m_groups.size(), sizeof(GpuGroupDesc),
                           m_groupDescBufferHandle, m_groupDescBufferCompat) && ok;
        ok = createBuffer(m_clusterVertices.data(), m_clusterVertices.size(), sizeof(uint32_t),
                           m_clusterVertexBufferHandle, m_clusterVertexBufferCompat) && ok;
        ok = createBuffer(m_clusterTriangles.data(), m_clusterTriangles.size(), sizeof(uint32_t),
                           m_clusterTriangleBufferHandle, m_clusterTriangleBufferCompat) && ok;
        ok = createBuffer(m_nodes.data(), m_nodes.size(), sizeof(GpuNodeDesc),
                           m_nodeDescBufferHandle, m_nodeDescBufferCompat) && ok;

        if (!ok)
        {
            Release();
            return false;
        }

        const size_t clusterBytes = m_clusters.size() * sizeof(GpuClusterDesc);
        const size_t groupBytes = m_groups.size() * sizeof(GpuGroupDesc);
        const size_t vertexBytes = m_clusterVertices.size() * sizeof(uint32_t);
        const size_t triangleBytes = m_clusterTriangles.size() * sizeof(uint32_t);
        const size_t nodeBytes = m_nodes.size() * sizeof(GpuNodeDesc);
        const size_t totalBytes = clusterBytes + groupBytes + vertexBytes + triangleBytes + nodeBytes;

        char buf[192];
        snprintf(buf, sizeof(buf),
            "[ClusterDagGpuBuffer] clusters=%u groups=%u vertices=%u triangles=%u nodes=%u bytes=%zu\n",
            static_cast<uint32_t>(m_clusters.size()), static_cast<uint32_t>(m_groups.size()),
            static_cast<uint32_t>(m_clusterVertices.size()), static_cast<uint32_t>(m_clusterTriangles.size()),
            static_cast<uint32_t>(m_nodes.size()), totalBytes);
        DebugLog(buf);

        return true;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterDagGpuBuffer::GetClusterDescGpuVA() const
    {
        Resource* r = GetClusterDescBufferResource();
        return r && r->IsValid() ? r->GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterDagGpuBuffer::GetGroupDescGpuVA() const
    {
        Resource* r = GetGroupDescBufferResource();
        return r && r->IsValid() ? r->GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterDagGpuBuffer::GetClusterVertexGpuVA() const
    {
        Resource* r = GetClusterVertexBufferResource();
        return r && r->IsValid() ? r->GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterDagGpuBuffer::GetClusterTriangleGpuVA() const
    {
        Resource* r = GetClusterTriangleBufferResource();
        return r && r->IsValid() ? r->GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterDagGpuBuffer::GetNodeDescGpuVA() const
    {
        Resource* r = GetNodeDescBufferResource();
        return r && r->IsValid() ? r->GetGPUVirtualAddress() : 0u;
    }
}
