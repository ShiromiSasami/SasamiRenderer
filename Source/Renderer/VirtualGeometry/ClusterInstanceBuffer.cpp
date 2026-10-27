#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterInstanceBuffer.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Resources/SrvDescriptorAllocator.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Structures/Vertex.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"

#include <cstdio>
#include <cstring>

namespace SasamiEngine::Render
{
    namespace
    {
        // The one set of rules deciding whether a draw item becomes a cluster instance. The counting
        // pass that sizes the buffer and the pass that fills it must agree exactly: a predicate that
        // drifts between them either under-allocates (truncating after all) or leaves a hole in the
        // written range that the GPU reads as a zeroed instance.
        const ClusterDagGpuBuffer::MeshRange* ResolveItem(const SceneSubmitter::DrawItem& item,
                                                          const ClusterDagGpuBuffer& clusters,
                                                          const std::vector<uint32_t>& vertexSrvIndex,
                                                          uint32_t invalidSrvIndex,
                                                          uint32_t& outVertexSrvIndex)
        {
            if (item.transparent)
            {
                return nullptr;
            }
            if (item.meshIndex >= clusters.GetMeshRangeCount())
            {
                return nullptr;
            }
            if (item.meshIndex >= vertexSrvIndex.size())
            {
                return nullptr;
            }

            const uint32_t srvIndex = vertexSrvIndex[item.meshIndex];
            if (srvIndex == invalidSrvIndex)
            {
                return nullptr;
            }

            const ClusterDagGpuBuffer::MeshRange& range = clusters.GetMeshRange(item.meshIndex);
            if (range.clusterCount == 0)
            {
                return nullptr;
            }

            outVertexSrvIndex = srvIndex;
            return &range;
        }
    }

    ClusterInstanceBuffer::~ClusterInstanceBuffer()
    {
        Release();
    }

    bool ClusterInstanceBuffer::Initialize(GraphicsDevice& device, uint32_t frameCount, uint32_t initialCapacity)
    {
        Release();

        if (frameCount == 0 || initialCapacity == 0)
        {
            return false;
        }

        if (!m_buffer.Initialize(device, frameCount, initialCapacity, sizeof(GpuClusterInstance), kMaxInstances,
                                 "ClusterInstanceBuffer"))
        {
            return false;
        }

        m_capacity = initialCapacity;
        return true;
    }

    void ClusterInstanceBuffer::Release()
    {
        m_buffer.Release();
        m_vertexSrvIndex.clear();
        m_vertexSrvBufferId.clear();
        m_sourceItemIndices.clear();

        m_capacity        = 0;
        m_instanceCount   = 0;
        m_required        = 0;
        m_overflowed      = false;
    }

    bool ClusterInstanceBuffer::IsValid() const
    {
        return m_buffer.IsValid();
    }

    bool ClusterInstanceBuffer::EnsureVertexSrvs(GraphicsDevice& device, SrvDescriptorAllocator& allocator, const MeshBuffer& meshBuffer)
    {
        const std::vector<MeshBuffer::GPUItem>& items = meshBuffer.Items();

        if (m_vertexSrvIndex.size() != items.size())
        {
            // The allocator is a simple bump allocator with no free(); the descriptors that were
            // allocated for the previous mesh count are just abandoned when the cache is rebuilt.
            m_vertexSrvIndex.assign(items.size(), kInvalidSrvIndex);
            m_vertexSrvBufferId.assign(items.size(), 0);
        }

        for (size_t i = 0; i < items.size(); ++i)
        {
            const MeshBuffer::GPUItem& item = items[i];
            // A ring-written mesh switches vertex buffers every frame, and this allocator never frees, so an
            // SRV per switch would run it dry. The cluster path never draws such a mesh (no DAG is built for
            // dynamicVertices), so it needs none.
            if (meshBuffer.HasDynamicVertices(i))
            {
                m_vertexSrvIndex[i] = kInvalidSrvIndex;
                continue;
            }
            // A scene re-sync can rebuild every vertex buffer while keeping the mesh count, so the
            // count alone does not prove a cached SRV still points at a live buffer. Handle ids are
            // never reused, so a matching id does.
            if (m_vertexSrvIndex[i] != kInvalidSrvIndex && m_vertexSrvBufferId[i] == item.rhiVb.id)
            {
                continue;
            }
            m_vertexSrvIndex[i] = kInvalidSrvIndex;

            if (!item.rhiVb.IsValid())
            {
                continue;
            }

            CpuDescriptorHandle cpu{};
            GpuDescriptorHandle gpu{};
            if (!allocator.Allocate(1, cpu, gpu))
            {
                return false;
            }

            const uint32_t stride = item.vbv.StrideInBytes != 0 ? item.vbv.StrideInBytes : sizeof(Vertex);

            RhiBufferViewDesc desc{};
            desc.type = RhiBufferViewType::Structured;
            desc.strideInBytes = stride;
            desc.sizeInBytes = static_cast<uint64_t>(stride) * item.vertexCount;
            desc.offset = 0;

            if (!device.CreateRhiBufferShaderResourceView(item.rhiVb, desc, RhiCpuDescriptorHandle{ cpu.ptr }))
            {
                return false;
            }

            m_vertexSrvIndex[i] = allocator.GetIndex(gpu);
            m_vertexSrvBufferId[i] = item.rhiVb.id;
        }

        return true;
    }

    uint32_t ClusterInstanceBuffer::Update(uint32_t frameIndex, const std::vector<SceneSubmitter::DrawItem>& items, const ClusterDagGpuBuffer& clusters)
    {
        m_instanceCount = 0;
        m_required      = 0;
        m_overflowed    = false;
        m_sourceItemIndices.clear();

        if (!IsValid() || frameIndex >= m_buffer.GetFrameCount())
        {
            return 0;
        }

        m_buffer.TickRetired();

        // Size the slot before writing it: counting first is what turns "drop the excess" into
        // "allocate for the excess", and the count is cheap next to the 96-byte copy per instance.
        uint32_t required = 0;
        for (const SceneSubmitter::DrawItem& item : items)
        {
            uint32_t vertexSrvIndex = kInvalidSrvIndex;
            if (ResolveItem(item, clusters, m_vertexSrvIndex, kInvalidSrvIndex, vertexSrvIndex) != nullptr)
            {
                ++required;
            }
        }
        m_required = required;

        m_sourceItemIndices.reserve(required);

        if (required > m_buffer.GetSlotCapacity(frameIndex) && !m_buffer.EnsureCapacity(frameIndex, required))
        {
            // The only path that still drops instances: the allocation failed or the 24-bit instance
            // index is exhausted. Reported through ClusterStreamingStats::overflowFlags, not just logged.
            m_overflowed = true;
            char buffer[176] = {};
            std::snprintf(buffer, sizeof(buffer),
                          "[ClusterInstanceBuffer] Update: %u instances requested but the slot holds %u; the excess is not drawn.\n",
                          required, m_buffer.GetSlotCapacity(frameIndex));
            DebugLog(buffer);
        }

        const uint32_t slotCapacity = m_buffer.GetSlotCapacity(frameIndex);
        m_capacity = slotCapacity;
        uint8_t* mapped = m_buffer.GetMapped(frameIndex);
        if (!mapped)
        {
            return 0;
        }

        GpuClusterInstance* dst = reinterpret_cast<GpuClusterInstance*>(mapped);
        uint32_t written = 0;

        for (size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
        {
            const SceneSubmitter::DrawItem& item = items[itemIndex];
            uint32_t vertexSrvIndex = kInvalidSrvIndex;
            const ClusterDagGpuBuffer::MeshRange* range =
                ResolveItem(item, clusters, m_vertexSrvIndex, kInvalidSrvIndex, vertexSrvIndex);
            if (range == nullptr)
            {
                continue;
            }

            if (written >= slotCapacity)
            {
                break;
            }

            GpuClusterInstance& inst = dst[written];
            std::memcpy(inst.model, item.model, sizeof(inst.model));
            inst.clusterOffset = range->clusterOffset;
            inst.clusterCount = range->clusterCount;
            inst.nodeOffset = range->nodeOffset;
            inst.rootNodeCount = range->rootNodeCount;
            inst.vertexSrvIndex = vertexSrvIndex;
            inst.pad[0] = 0;
            inst.pad[1] = 0;
            inst.pad[2] = 0;
            m_sourceItemIndices.push_back(static_cast<uint32_t>(itemIndex));
            ++written;
        }

        m_instanceCount = written;
        return written;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterInstanceBuffer::GetGpuVA(uint32_t frameIndex) const
    {
        return m_buffer.GetGpuVA(frameIndex);
    }
}
