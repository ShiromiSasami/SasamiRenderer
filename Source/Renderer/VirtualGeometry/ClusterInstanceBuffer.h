#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/PerFrameGrowableUploadBuffer.h"
#include "Renderer/Scene/SceneSubmitter.h"
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    class MeshBuffer;
    class ClusterDagGpuBuffer;
    class SrvDescriptorAllocator;

    struct GpuClusterInstance
    {
        float    model[16];       // row-major, same convention as DrawItem::model
        uint32_t clusterOffset;   // absolute cluster range of the mesh
        uint32_t clusterCount;
        uint32_t nodeOffset;      // absolute node range; roots are nodeOffset .. nodeOffset+rootNodeCount
        uint32_t rootNodeCount;
        uint32_t vertexSrvIndex;  // shader-visible heap index of the mesh's StructuredBuffer<Vertex> SRV
        uint32_t pad[3];
    };
    static_assert(sizeof(GpuClusterInstance) == 96);

    // Per-frame list of cluster-drawn instances (static, opaque, with clusters) for the GPU culling pass.
    //
    // Each frame slot owns its own upload buffer and each buffer grows on demand: Update counts the
    // eligible instances first and reallocates the slot it is about to write when the draw list no
    // longer fits, instead of truncating it. That keeps the steady-state footprint at the initial
    // capacity (a Bistro-sized scene never reallocates) while a procedurally populated scene of
    // 100k instances still gets every instance to the GPU.
    //
    // The slot growth and buffer-retirement mechanics live in PerFrameGrowableUploadBuffer: a
    // reallocation never frees the buffer the GPU might still be reading.
    class ClusterInstanceBuffer
    {
    public:
        // Hard ceiling on the instance count, mirroring kVisibleInstanceBits in
        // MeshShader_Common.hlsli: the visible-list entry carries the instance index in 24 bits, so
        // a 16,777,216th instance would be packed wrapped and drawn with another instance's matrix
        // (ClusterSelect_CS raises kOverflowInstanceIndex for exactly that). The buffer therefore
        // refuses to grow past it and reports capacity overflow instead of growing into corruption.
        // Keep in step with kVisibleInstanceBits: widening one without the other reintroduces the
        // silent mispacking that the flag was added to catch.
        static constexpr uint32_t kMaxInstances = 1u << 24;

        ~ClusterInstanceBuffer();

        // initialCapacity instances per frame slot; slots grow from here as the draw list demands.
        bool Initialize(GraphicsDevice& device, uint32_t frameCount, uint32_t initialCapacity);
        void Release();
        bool IsValid() const;

        // Creates (once per mesh, cached by mesh index) a StructuredBuffer<Vertex> SRV in the shader-visible heap.
        // Rebuilds the cache when the mesh count changes. Returns false if the heap is exhausted.
        bool EnsureVertexSrvs(GraphicsDevice& device, SrvDescriptorAllocator& allocator, const MeshBuffer& meshBuffer);

        // Writes this frame's instances into slot frameIndex; skips transparent items, items whose mesh has no
        // clusters, and items without a vertex SRV. Grows the slot when the eligible instances do not fit and
        // returns the number written. Only a growth that cannot be satisfied (allocation failure or kMaxInstances)
        // truncates the list, and that raises HasOverflowed.
        uint32_t Update(uint32_t frameIndex, const std::vector<SceneSubmitter::DrawItem>& items, const ClusterDagGpuBuffer& clusters);

        D3D12_GPU_VIRTUAL_ADDRESS GetGpuVA(uint32_t frameIndex) const;

        uint32_t GetInstanceCount() const { return m_instanceCount; }
        uint32_t GetCapacity() const { return m_capacity; }          // capacity of the slot the last Update wrote
        uint32_t GetRequiredCapacity() const { return m_required; }  // eligible instances the last Update counted

        // True when the last Update could not fit every eligible instance. The renderer forwards this
        // into ClusterStreamingStats::overflowFlags (kOverflowInstanceCapacity) so a dropped instance
        // is reported through the same channel as the GPU-side overflows, never only to the log.
        bool HasOverflowed() const { return m_overflowed; }

        // DrawItem index (into the items vector passed to the last Update) of each written instance, in instance order.
        const std::vector<uint32_t>& GetSourceItemIndices() const { return m_sourceItemIndices; }

    private:
        static constexpr uint32_t kInvalidSrvIndex = 0xffffffffu;

        PerFrameGrowableUploadBuffer m_buffer;

        uint32_t m_capacity        = 0;
        uint32_t m_instanceCount   = 0;
        uint32_t m_required        = 0;
        bool     m_overflowed      = false;

        std::vector<uint32_t> m_sourceItemIndices;

        // Vertex-buffer SRV heap index per mesh (kInvalidSrvIndex when not yet created).
        std::vector<uint32_t> m_vertexSrvIndex;
        // RhiBufferHandle id each cached SRV was created for (0 = none).
        std::vector<uint64_t> m_vertexSrvBufferId;
    };
}

namespace SasamiEngine { using namespace Render; }
