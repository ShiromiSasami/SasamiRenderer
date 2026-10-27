#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    class ClusterPageStore;

    struct ClusterPagePoolStats
    {
        uint64_t poolBytes = 0, usedBytes = 0, ringBytesPerFrame = 0;
        uint32_t residentPages = 0, loadsThisFrame = 0, evictionsThisFrame = 0;
        uint32_t deferredLoads = 0;      // pages desired but not loaded this frame
        uint64_t totalLoads = 0, totalEvictions = 0, totalBytesUploaded = 0;

        // Why this frame stopped loading. One counter cannot answer it: "the pool is full" and "the
        // pool has the bytes but not in one piece" call for opposite fixes (raise the budget vs.
        // compact the free list), and a deferral that is only waiting on an in-flight free fixes
        // itself next frame.
        //
        // The first failure ends the frame, so at most one of these is set per frame and they do NOT
        // sum to deferredLoads -- the remaining pages are skipped for the same reason and counted
        // there only. Measured on Bistro with SASAMI_PAGE_POOL_MB=8: deferred=6570(full=1).
        uint32_t deferredByRing = 0;           // this frame's upload ring is full
        uint32_t deferredByPoolFull = 0;       // not enough free bytes in the pool at all
        uint32_t deferredByPendingFree = 0;    // enough bytes, but they are still held for in-flight frames
        uint32_t deferredByFragmentation = 0;  // enough free bytes, no single block large enough

        uint32_t freeBlocks = 0;         // allocator fragmentation indicator
        uint32_t largestFreeBlock = 0;   // bytes in the biggest contiguous free block
        uint64_t freeBytes = 0;          // bytes in the free list (allocatable now)
        uint64_t pendingFreeBytes = 0;   // freed but not yet reusable (held frameCount frames)
        // The frame that loaded the most pages since the last Reset. Every field above is per-frame and
        // reads zero on almost every sample, so polling from outside practically never lands on a load
        // frame -- the peak is the only way to see what a page swap actually costs.
        uint32_t peakLoadPages = 0;
        double   peakLoadApplyMs = 0.0;
        double   peakLoadCopyMs = 0.0;

        double   applyMilliseconds = 0.0;// CPU time of the last ApplyResidency (whole call)
        double   copyMilliseconds = 0.0; // of which: the page memcpy into the upload ring
                                         // Split out because the two scale with different things -- the
                                         // residency scan with groupCount, the copies with pages loaded --
                                         // and deciding whether to make the loads asynchronous needs to know
                                         // which one the load-frame cost actually is.
    };

    // GPU residency pool of group pages (Nanite streams fixed-size pages into a GPU pool; here pages are variable
    // sized, so a first-fit free-list allocator with coalescing is used). Freed regions are reused only after
    // frameCount frames so in-flight GPU work never reads an overwritten page.
    class ClusterPagePool
    {
    public:
        static constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

        ~ClusterPagePool();

        bool Initialize(GraphicsDevice& device, uint32_t frameCount, uint64_t poolBytes, uint64_t uploadRingBytesPerFrame);
        void Release();
        bool IsValid() const;

        // Forgets every resident page (call when the page store is rebuilt). groupCount sizes the offset table.
        void Reset(uint32_t groupCount);

        // Diffs desiredResidentBits against the pool's own resident set: frees pages no longer desired, then loads
        // desired-but-absent pages (allocating in the pool, memcpy into this frame's upload ring, CopyBufferRegion
        // into the pool) until the ring or the pool is exhausted. Records the transitions on cmdList
        // (pool: NON_PIXEL_SHADER_RESOURCE -> COPY_DEST -> NON_PIXEL_SHADER_RESOURCE, only when there are copies)
        // and uploads the offset table into frame slot frameIndex. Returns the number of pages loaded.
        uint32_t ApplyResidency(CommandList& cmdList, uint32_t frameIndex, const std::vector<uint32_t>& desiredResidentBits,
                                const ClusterPageStore& pages);

        D3D12_GPU_VIRTUAL_ADDRESS GetPoolVA() const;
        D3D12_GPU_VIRTUAL_ADDRESS GetOffsetTableVA(uint32_t frameIndex) const;   // uint per group, kInvalidOffset = not resident
        bool IsGroupResident(uint32_t group) const;
        const std::vector<uint32_t>& GetResidentBits() const { return m_residentBits; }
        const ClusterPagePoolStats& GetStats() const { return m_stats; }

        // Clears the peak-load record. The boot fill is the largest swap the pool ever sees and it
        // runs while the scene is still loading, so it both masks later swaps and is far too noisy
        // to compare against (the same 2558-page fill measured 3.5-5.8 ms across launches).
        // Reset after the scene settles to time a swap on its own.
        void ResetPeakLoadStats();


        // "pool=..MB used=..MB (xx.x%) pages=.. loads=..(+total) evictions=..(+total) deferred=..(ring/full/pending/frag)
        //  ring=..MB free=..MB in .. blocks (largest ..KB) pending=..MB cpu=..ms"
        std::string FormatReport() const;

    private:
        static constexpr uint32_t kAllocAlignment = 16u;
        static uint32_t AlignUp(uint32_t value);
        static uint64_t AlignUp64(uint64_t value);

        struct FreeBlock
        {
            uint32_t offset = 0;
            uint32_t size = 0;
        };

        struct PendingFree
        {
            uint32_t offset = 0;
            uint32_t size = 0;
            uint64_t reusableAtFrame = 0;
        };

        struct PendingCopy
        {
            uint32_t poolOffset = 0;
            uint64_t ringCursor = 0;
            uint32_t size = 0;
            const uint8_t* src = nullptr;
        };

        struct UploadRing
        {
            RhiBufferHandle handle{};
            Resource* compat = nullptr;
            void* mapped = nullptr;
            uint64_t cursor = 0;
        };

        struct OffsetTableSlot
        {
            RhiBufferHandle handle{};
            Resource* compat = nullptr;
            void* mapped = nullptr;
            uint64_t generation = 0;   // m_tableGeneration this slot was last filled with
        };

        bool CreatePoolBuffer();
        bool CreateRings();
        bool EnsureOffsetTableCapacity(uint32_t groupCount);

        struct FreeListSummary
        {
            uint32_t blocks = 0;
            uint32_t largest = 0;
            uint64_t bytes = 0;
        };
        FreeListSummary SummarizeFreeList() const;
        uint64_t PendingFreeBytes() const;
        void RefreshFreeListStats();
        void ResetPerFrameStats();

        bool AllocateFromPool(uint32_t sizeBytes, uint32_t& outOffset);
        void DeferFree(uint32_t offset, uint32_t size);
        void MaturePendingFrees();
        void InsertFreeBlock(uint32_t offset, uint32_t size);

        bool GetResidentBit(uint32_t group) const;
        void SetResidentBit(uint32_t group, bool value);
        bool GetDesiredBit(const std::vector<uint32_t>& bits, uint32_t group) const;

        // pool buffer (GpuOnly, ShaderResource|CopyDest, initial NON_PIXEL_SHADER_RESOURCE state tracked), per-frame upload ring
        // (CpuToGpu, mapped) with a write cursor reset each ApplyResidency, per-frame offset-table upload buffers (CpuToGpu, mapped,
        // groupCount*4 bytes rounded up; recreate when groupCount grows), CPU offset table, resident bitset, free list of
        // {offset,size} sorted by offset with coalescing, pending frees {offset,size,reusableAtFrame}, frame counter.
        GraphicsDevice* m_device = nullptr;
        uint32_t m_frameCount = 0;
        bool m_valid = false;

        uint64_t m_poolBytes = 0;
        uint64_t m_ringBytesPerFrame = 0;

        RhiBufferHandle m_poolHandle{};
        Resource* m_poolCompat = nullptr;
        D3D12_RESOURCE_STATES m_poolState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        std::vector<UploadRing> m_rings;

        std::vector<OffsetTableSlot> m_offsetTables;
        uint32_t m_offsetTableCapacityGroups = 0;
        uint64_t m_offsetTableByteSize = 0;

        std::vector<uint32_t> m_offsetTableCpu;
        // Skipping an unchanged frame: the per-frame scan is O(groupCount) and dominates the cost
        // once the resident set settles (measured 0.55-0.70 ms with zero page copies on Bistro).
        std::vector<uint32_t> m_lastDesiredBits;
        uint64_t m_tableGeneration = 1;   // bumped whenever m_offsetTableCpu changes

        std::vector<uint32_t> m_residentBits;

        std::vector<FreeBlock> m_freeList;
        std::vector<PendingFree> m_pendingFrees;
        uint64_t m_currentFrame = 0;

        bool m_hasLoggedDeferral = false;
        uint64_t m_lastDeferralLogFrame = 0;

        ClusterPagePoolStats m_stats{};
    };
}

namespace SasamiEngine { using namespace Render; }
