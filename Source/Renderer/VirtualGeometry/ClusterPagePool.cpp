#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterPagePool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "d3dx12.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/VirtualGeometry/ClusterPageStore.h"

namespace
{
    constexpr uint64_t kDeferralLogIntervalFrames = 300ull;
}

namespace SasamiEngine::Render
{
    ClusterPagePool::~ClusterPagePool()
    {
        Release();
    }

    uint32_t ClusterPagePool::AlignUp(uint32_t value)
    {
        return (value + (kAllocAlignment - 1u)) & ~(kAllocAlignment - 1u);
    }

    uint64_t ClusterPagePool::AlignUp64(uint64_t value)
    {
        return (value + (uint64_t{kAllocAlignment} - 1u)) & ~(uint64_t{kAllocAlignment} - 1u);
    }

    bool ClusterPagePool::Initialize(GraphicsDevice& device, uint32_t frameCount, uint64_t poolBytes, uint64_t uploadRingBytesPerFrame)
    {
        Release();

        if (frameCount == 0u || poolBytes == 0u || uploadRingBytesPerFrame == 0u) {
            DebugLog("ClusterPagePool::Initialize: frameCount, poolBytes and uploadRingBytesPerFrame must be non-zero.\n");
            return false;
        }

        m_device = &device;
        m_frameCount = frameCount;
        m_poolBytes = poolBytes;
        m_ringBytesPerFrame = uploadRingBytesPerFrame;

        if (!CreatePoolBuffer()) {
            DebugLog("ClusterPagePool::Initialize: failed to create the pool buffer.\n");
            Release();
            return false;
        }

        if (!CreateRings()) {
            DebugLog("ClusterPagePool::Initialize: failed to create the upload rings.\n");
            Release();
            return false;
        }

        if (!EnsureOffsetTableCapacity(0u)) {
            DebugLog("ClusterPagePool::Initialize: failed to create the offset table.\n");
            Release();
            return false;
        }

        m_freeList.clear();
        m_freeList.push_back(FreeBlock{0u, static_cast<uint32_t>(poolBytes)});

        m_stats = ClusterPagePoolStats{};
        m_stats.poolBytes = poolBytes;
        m_stats.ringBytesPerFrame = uploadRingBytesPerFrame;
        m_stats.freeBlocks = 1u;
        m_stats.largestFreeBlock = m_freeList.front().size;
        m_stats.freeBytes = m_freeList.front().size;
        m_stats.pendingFreeBytes = 0ull;

        m_valid = true;
        return true;
    }

    void ClusterPagePool::Release()
    {
        for (UploadRing& ring : m_rings) {
            if (ring.mapped && ring.compat) {
                ring.compat->Unmap(0, nullptr);
            }
            if (m_device && ring.handle.IsValid()) {
                m_device->DestroyRhiResource(ring.handle);
            }
        }
        m_rings.clear();

        for (OffsetTableSlot& slot : m_offsetTables) {
            if (slot.mapped && slot.compat) {
                slot.compat->Unmap(0, nullptr);
            }
            if (m_device && slot.handle.IsValid()) {
                m_device->DestroyRhiResource(slot.handle);
            }
        }
        m_offsetTables.clear();
        m_offsetTableCapacityGroups = 0;
        m_offsetTableByteSize = 0;

        if (m_device && m_poolHandle.IsValid()) {
            m_device->DestroyRhiResource(m_poolHandle);
        }
        m_poolHandle = {};
        m_poolCompat = nullptr;
        m_poolState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        m_offsetTableCpu.clear();
        m_residentBits.clear();
        m_freeList.clear();
        m_pendingFrees.clear();
        m_currentFrame = 0;

        m_hasLoggedDeferral = false;
        m_lastDeferralLogFrame = 0;

        m_poolBytes = 0;
        m_ringBytesPerFrame = 0;
        m_frameCount = 0;
        m_valid = false;
        m_device = nullptr;
        m_stats = ClusterPagePoolStats{};
    }

    bool ClusterPagePool::IsValid() const
    {
        return m_valid;
    }

    bool ClusterPagePool::CreatePoolBuffer()
    {
        if (!m_device) return false;

        RhiBufferDesc desc{};
        desc.sizeInBytes = m_poolBytes;
        desc.strideInBytes = 4u;
        desc.usage = RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::CopyDest;
        desc.memoryUsage = RhiMemoryUsage::GpuOnly;
        desc.initialState = RhiResourceState::ShaderResource;

        m_poolHandle = m_device->CreateRhiBuffer(desc);
        m_poolCompat = m_device->GetD3D12CompatibilityResource(m_poolHandle);
        m_poolState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return m_poolCompat != nullptr && m_poolCompat->IsValid();
    }

    bool ClusterPagePool::CreateRings()
    {
        if (!m_device) return false;

        m_rings.assign(m_frameCount, UploadRing{});
        for (UploadRing& ring : m_rings) {
            RhiBufferDesc desc{};
            desc.sizeInBytes = m_ringBytesPerFrame;
            desc.strideInBytes = 4u;
            desc.usage = RhiBufferUsageFlags::CopySource;
            desc.memoryUsage = RhiMemoryUsage::CpuToGpu;
            desc.initialState = RhiResourceState::CopySource;

            ring.handle = m_device->CreateRhiBuffer(desc);
            ring.compat = m_device->GetD3D12CompatibilityResource(ring.handle);
            if (!ring.compat || !ring.compat->IsValid()) {
                return false;
            }
            const D3D12_RANGE emptyRange{0, 0};
            if (FAILED(ring.compat->Map(0, &emptyRange, &ring.mapped)) || !ring.mapped) {
                return false;
            }
        }
        return true;
    }

    bool ClusterPagePool::EnsureOffsetTableCapacity(uint32_t groupCount)
    {
        if (!m_offsetTables.empty() && groupCount <= m_offsetTableCapacityGroups) {
            return true;
        }
        if (!m_device) return false;

        for (OffsetTableSlot& slot : m_offsetTables) {
            if (slot.mapped && slot.compat) {
                slot.compat->Unmap(0, nullptr);
            }
            if (slot.handle.IsValid()) {
                m_device->DestroyRhiResource(slot.handle);
            }
        }
        m_offsetTables.assign(m_frameCount, OffsetTableSlot{});

        const uint64_t byteSize = (std::max)(AlignUp64(static_cast<uint64_t>(groupCount) * 4ull), uint64_t{kAllocAlignment});

        for (OffsetTableSlot& slot : m_offsetTables) {
            RhiBufferDesc desc{};
            desc.sizeInBytes = byteSize;
            desc.strideInBytes = 4u;
            desc.usage = RhiBufferUsageFlags::ShaderResource;
            desc.memoryUsage = RhiMemoryUsage::CpuToGpu;
            desc.initialState = RhiResourceState::ShaderResource;

            slot.handle = m_device->CreateRhiBuffer(desc);
            slot.compat = m_device->GetD3D12CompatibilityResource(slot.handle);
            if (!slot.compat || !slot.compat->IsValid()) {
                return false;
            }
            const D3D12_RANGE emptyRange{0, 0};
            if (FAILED(slot.compat->Map(0, &emptyRange, &slot.mapped)) || !slot.mapped) {
                return false;
            }
            std::memset(slot.mapped, 0xFF, static_cast<size_t>(byteSize));  // 0xFFFFFFFF per uint == kInvalidOffset
        }

        m_offsetTableCapacityGroups = groupCount;
        m_offsetTableByteSize = byteSize;
        return true;
    }

    void ClusterPagePool::Reset(uint32_t groupCount)
    {
        if (!m_valid) return;

        if (!EnsureOffsetTableCapacity(groupCount)) {
            return;
        }

        m_offsetTableCpu.assign(groupCount, kInvalidOffset);
        m_lastDesiredBits.clear();
        ++m_tableGeneration;
        m_residentBits.assign((groupCount + 31u) / 32u, 0u);

        m_freeList.clear();
        m_freeList.push_back(FreeBlock{0u, static_cast<uint32_t>(m_poolBytes)});
        m_pendingFrees.clear();
        m_currentFrame = 0;

        m_stats = ClusterPagePoolStats{};
        m_stats.poolBytes = m_poolBytes;
        m_stats.ringBytesPerFrame = m_ringBytesPerFrame;
        m_stats.freeBlocks = 1u;
        m_stats.largestFreeBlock = m_freeList.front().size;
        m_stats.freeBytes = m_freeList.front().size;
        m_stats.pendingFreeBytes = 0ull;
    }

    ClusterPagePool::FreeListSummary ClusterPagePool::SummarizeFreeList() const
    {
        FreeListSummary summary{};
        summary.blocks = static_cast<uint32_t>(m_freeList.size());
        for (const FreeBlock& block : m_freeList) {
            summary.largest = std::max(summary.largest, block.size);
            summary.bytes += block.size;
        }
        return summary;
    }

    uint64_t ClusterPagePool::PendingFreeBytes() const
    {
        return std::accumulate(m_pendingFrees.begin(), m_pendingFrees.end(), uint64_t{0},
                               [](uint64_t total, const PendingFree& entry) { return total + entry.size; });
    }

    void ClusterPagePool::ResetPeakLoadStats()
    {
        m_stats.peakLoadPages = 0u;
        m_stats.peakLoadApplyMs = 0.0;
        m_stats.peakLoadCopyMs = 0.0;
    }

    void ClusterPagePool::RefreshFreeListStats()
    {
        const FreeListSummary summary = SummarizeFreeList();
        m_stats.freeBlocks = summary.blocks;
        m_stats.largestFreeBlock = summary.largest;
        m_stats.freeBytes = summary.bytes;
        m_stats.pendingFreeBytes = PendingFreeBytes();
    }

    void ClusterPagePool::ResetPerFrameStats()
    {
        m_stats.loadsThisFrame = 0u;
        m_stats.evictionsThisFrame = 0u;
        m_stats.deferredLoads = 0u;
        m_stats.deferredByRing = 0u;
        m_stats.deferredByPoolFull = 0u;
        m_stats.deferredByPendingFree = 0u;
        m_stats.deferredByFragmentation = 0u;
    }

    bool ClusterPagePool::AllocateFromPool(uint32_t sizeBytes, uint32_t& outOffset)
    {
        for (size_t i = 0; i < m_freeList.size(); ++i) {
            FreeBlock& block = m_freeList[i];
            if (block.size < sizeBytes) {
                continue;
            }
            outOffset = block.offset;
            if (block.size == sizeBytes) {
                m_freeList.erase(m_freeList.begin() + static_cast<ptrdiff_t>(i));
            } else {
                block.offset += sizeBytes;
                block.size -= sizeBytes;
            }
            return true;
        }
        return false;
    }

    void ClusterPagePool::DeferFree(uint32_t offset, uint32_t size)
    {
        const uint32_t alignedSize = AlignUp(size);
        m_pendingFrees.push_back(PendingFree{offset, alignedSize, m_currentFrame + m_frameCount});
    }

    void ClusterPagePool::InsertFreeBlock(uint32_t offset, uint32_t size)
    {
        auto it = std::lower_bound(m_freeList.begin(), m_freeList.end(), offset,
            [](const FreeBlock& block, uint32_t value) { return block.offset < value; });
        it = m_freeList.insert(it, FreeBlock{offset, size});

        auto next = it + 1;
        if (next != m_freeList.end() && it->offset + it->size == next->offset) {
            it->size += next->size;
            m_freeList.erase(next);
        }
        if (it != m_freeList.begin()) {
            auto prev = it - 1;
            if (prev->offset + prev->size == it->offset) {
                prev->size += it->size;
                m_freeList.erase(it);
            }
        }
    }

    void ClusterPagePool::MaturePendingFrees()
    {
        if (m_pendingFrees.empty()) return;

        std::vector<PendingFree> stillPending;
        stillPending.reserve(m_pendingFrees.size());
        for (const PendingFree& pending : m_pendingFrees) {
            if (pending.reusableAtFrame <= m_currentFrame) {
                InsertFreeBlock(pending.offset, pending.size);
            } else {
                stillPending.push_back(pending);
            }
        }
        m_pendingFrees.swap(stillPending);
    }

    bool ClusterPagePool::GetResidentBit(uint32_t group) const
    {
        const uint32_t word = group / 32u;
        if (word >= m_residentBits.size()) return false;
        return (m_residentBits[word] & (1u << (group % 32u))) != 0u;
    }

    void ClusterPagePool::SetResidentBit(uint32_t group, bool value)
    {
        const uint32_t word = group / 32u;
        if (word >= m_residentBits.size()) return;
        if (value) {
            m_residentBits[word] |= (1u << (group % 32u));
        } else {
            m_residentBits[word] &= ~(1u << (group % 32u));
        }
    }

    bool ClusterPagePool::GetDesiredBit(const std::vector<uint32_t>& bits, uint32_t group) const
    {
        const uint32_t word = group / 32u;
        if (word >= bits.size()) return false;
        return (bits[word] & (1u << (group % 32u))) != 0u;
    }

    uint32_t ClusterPagePool::ApplyResidency(CommandList& cmdList, uint32_t frameIndex, const std::vector<uint32_t>& desiredResidentBits,
                                             const ClusterPageStore& pages)
    {
        if (!m_valid || frameIndex >= m_frameCount) {
            return 0u;
        }

        // Timed so the cost of the synchronous page memcpy into the upload ring is visible:
        // moving it onto the JobSystem only pays off once this shows up in the frame budget.
        const auto applyStart = std::chrono::steady_clock::now();

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl || !m_poolCompat || !m_poolCompat->IsValid()) {
            return 0u;
        }

        MaturePendingFrees();
        // Before the early-out below, not only on the churning path: once residency settles every
        // frame takes that exit, and a free-list snapshot taken only on the other path would report
        // the last churning frame's numbers forever (pending frees that have long since matured).
        RefreshFreeListStats();

        // Nothing to do when the desired set has not moved and this frame slot already holds the
        // current table: the scan below walks every group, which is pure overhead once residency
        // settles (the common case at an unlimited budget).
        OffsetTableSlot& earlyTable = m_offsetTables[frameIndex];
        if (desiredResidentBits == m_lastDesiredBits && earlyTable.generation == m_tableGeneration) {
            ResetPerFrameStats();
            m_stats.copyMilliseconds = 0.0;
            m_stats.applyMilliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count();
            ++m_currentFrame;
            return 0u;
        }

        const uint32_t groupCount = static_cast<uint32_t>(m_offsetTableCpu.size());

        ResetPerFrameStats();

        // Free pages no longer desired.
        for (uint32_t group = 0; group < groupCount; ++group) {
            if (!GetResidentBit(group) || GetDesiredBit(desiredResidentBits, group)) {
                continue;
            }

            const uint32_t offset = m_offsetTableCpu[group];
            if (offset != kInvalidOffset && group < pages.GetPageCount()) {
                const uint32_t size = pages.GetPage(group).size;
                DeferFree(offset, size);
                m_stats.usedBytes -= AlignUp64(size);
                if (m_stats.residentPages > 0u) {
                    --m_stats.residentPages;
                }
            }

            SetResidentBit(group, false);
            m_offsetTableCpu[group] = kInvalidOffset;
            ++m_tableGeneration;
            ++m_stats.evictionsThisFrame;
            ++m_stats.totalEvictions;
        }

        // Load desired-but-absent pages in ascending group order until the ring or the pool is exhausted.
        UploadRing& ring = m_rings[frameIndex];
        ring.cursor = 0;

        std::vector<PendingCopy> copies;

        bool stopped = false;
        for (uint32_t group = 0; group < groupCount; ++group) {
            if (!GetDesiredBit(desiredResidentBits, group) || GetResidentBit(group)) {
                continue;
            }
            if (group >= pages.GetPageCount()) {
                continue;   // no page data built for this group
            }
            if (stopped) {
                ++m_stats.deferredLoads;   // cause already recorded by the load that stopped the frame
                continue;
            }

            const ClusterPageEntry& entry = pages.GetPage(group);
            const uint32_t alignedSize = AlignUp(entry.size);
            const uint64_t ringStart = AlignUp64(ring.cursor);

            uint32_t poolOffset = 0;
            const bool ringFull = ringStart + alignedSize > m_ringBytesPerFrame;
            if (ringFull || !AllocateFromPool(alignedSize, poolOffset)) {
                stopped = true;
                ++m_stats.deferredLoads;
                if (ringFull) {
                    ++m_stats.deferredByRing;
                } else {
                    // AllocateFromPool is first-fit over the whole free list, so it fails with enough
                    // free bytes only when none of them are contiguous. Bytes still held for in-flight
                    // frames are not fragmentation -- that deferral resolves itself within frameCount.
                    const FreeListSummary summary = SummarizeFreeList();
                    if (summary.bytes >= alignedSize) {
                        ++m_stats.deferredByFragmentation;
                    } else if (summary.bytes + PendingFreeBytes() >= alignedSize) {
                        ++m_stats.deferredByPendingFree;
                    } else {
                        ++m_stats.deferredByPoolFull;
                    }
                }
                continue;
            }

            // The copy itself is deferred to the parallel pass; allocation order stays serial
            // so pool offsets and the ring cursor remain deterministic.
            copies.push_back(PendingCopy{poolOffset, ringStart, entry.size, pages.GetPageBytes(group).data()});
            ring.cursor = ringStart + alignedSize;

            m_offsetTableCpu[group] = poolOffset;
            ++m_tableGeneration;
            SetResidentBit(group, true);
            ++m_stats.loadsThisFrame;
            ++m_stats.totalLoads;
            ++m_stats.residentPages;
            m_stats.usedBytes += alignedSize;
            m_stats.totalBytesUploaded += entry.size;
        }

        // Copies are issued serially, and an A/B on 2026-09-13 says they should stay that way.
        // Steady-state swaps are capped at 256 pages per frame by ClusterResidencyManager, and at that
        // size a chunked parallel memcpy (one job per worker, not the one-job-per-page ParallelFor
        // default) was never faster: serial 0.322/0.213/0.177 ms against parallel 1.242/0.307/0.271 ms.
        // Job kick and worker wake-up cost more than ~600 KB of memcpy.
        //
        // The only frame big enough to pay for threads is the boot fill (2558 pages, copies 54-65% of
        // ApplyResidency), which happens once and whose total varies 3.5-5.8 ms across launches from
        // concurrent scene loading -- too noisy to optimise against, and not a frame the user waits on
        // twice. peakLoad= in render.poolstats is the way to re-check this if the 256 cap ever rises.
        const auto copyStart = std::chrono::steady_clock::now();
        for (const PendingCopy& copy : copies) {
            std::memcpy(static_cast<uint8_t*>(ring.mapped) + copy.ringCursor, copy.src, copy.size);
        }
        m_stats.copyMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStart).count();

        if (!copies.empty()) {
            const D3D12_RESOURCE_BARRIER toCopyDest =
                CD3DX12_RESOURCE_BARRIER::Transition(m_poolCompat->Get(), m_poolState, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->ResourceBarrier(1, &toCopyDest);
            m_poolState = D3D12_RESOURCE_STATE_COPY_DEST;

            for (const PendingCopy& copy : copies) {
                cl->CopyBufferRegion(m_poolCompat->Get(), copy.poolOffset, ring.compat->Get(), copy.ringCursor, copy.size);
            }

            const D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                m_poolCompat->Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl->ResourceBarrier(1, &toSrv);
            m_poolState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        OffsetTableSlot& tableSlot = m_offsetTables[frameIndex];
        if (tableSlot.mapped && !m_offsetTableCpu.empty()) {
            std::memcpy(tableSlot.mapped, m_offsetTableCpu.data(), m_offsetTableCpu.size() * sizeof(uint32_t));
            tableSlot.generation = m_tableGeneration;
            m_lastDesiredBits = desiredResidentBits;
        }

        RefreshFreeListStats();

        if (m_stats.deferredLoads > 0u &&
            (!m_hasLoggedDeferral || (m_currentFrame - m_lastDeferralLogFrame) >= kDeferralLogIntervalFrames)) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                "ClusterPagePool::ApplyResidency: %u page load(s) deferred (ring=%u poolFull=%u pendingFree=%u fragmentation=%u).\n",
                m_stats.deferredLoads, m_stats.deferredByRing, m_stats.deferredByPoolFull,
                m_stats.deferredByPendingFree, m_stats.deferredByFragmentation);
            DebugLog(buffer);
            m_hasLoggedDeferral = true;
            m_lastDeferralLogFrame = m_currentFrame;
        }

        m_stats.applyMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count();

        if (m_stats.loadsThisFrame > m_stats.peakLoadPages) {
            m_stats.peakLoadPages = m_stats.loadsThisFrame;
            m_stats.peakLoadApplyMs = m_stats.applyMilliseconds;
            m_stats.peakLoadCopyMs = m_stats.copyMilliseconds;
        }

        ++m_currentFrame;
        return m_stats.loadsThisFrame;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterPagePool::GetPoolVA() const
    {
        return (m_poolCompat && m_poolCompat->IsValid()) ? m_poolCompat->GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterPagePool::GetOffsetTableVA(uint32_t frameIndex) const
    {
        if (frameIndex >= m_offsetTables.size()) return 0u;
        const OffsetTableSlot& slot = m_offsetTables[frameIndex];
        return (slot.compat && slot.compat->IsValid()) ? slot.compat->GetGPUVirtualAddress() : 0u;
    }

    bool ClusterPagePool::IsGroupResident(uint32_t group) const
    {
        return GetResidentBit(group);
    }

    std::string ClusterPagePool::FormatReport() const
    {
        const double poolMB = static_cast<double>(m_stats.poolBytes) / (1024.0 * 1024.0);
        const double usedMB = static_cast<double>(m_stats.usedBytes) / (1024.0 * 1024.0);
        const double usedPct = m_stats.poolBytes > 0u
            ? (static_cast<double>(m_stats.usedBytes) / static_cast<double>(m_stats.poolBytes)) * 100.0
            : 0.0;
        const double ringMB = static_cast<double>(m_stats.ringBytesPerFrame) / (1024.0 * 1024.0);

        const double freeMB = static_cast<double>(m_stats.freeBytes) / (1024.0 * 1024.0);
        const double pendingMB = static_cast<double>(m_stats.pendingFreeBytes) / (1024.0 * 1024.0);
        const double largestKB = static_cast<double>(m_stats.largestFreeBlock) / 1024.0;

        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
            "pool=%.1fMB used=%.1fMB (%.1f%%) pages=%u loads=%u(+%llu) evictions=%u(+%llu) "
            "deferred=%u(ring=%u full=%u pending=%u frag=%u) ring=%.1fMB "
            "free=%.1fMB in %u blocks (largest=%.1fKB) pendingFree=%.1fMB cpu=%.3fms(copy=%.3fms) "
            "peakLoad=%u pages in %.3fms(copy=%.3fms)",
            poolMB, usedMB, usedPct, m_stats.residentPages,
            m_stats.loadsThisFrame, static_cast<unsigned long long>(m_stats.totalLoads),
            m_stats.evictionsThisFrame, static_cast<unsigned long long>(m_stats.totalEvictions),
            m_stats.deferredLoads, m_stats.deferredByRing, m_stats.deferredByPoolFull,
            m_stats.deferredByPendingFree, m_stats.deferredByFragmentation, ringMB,
            freeMB, m_stats.freeBlocks, largestKB, pendingMB, m_stats.applyMilliseconds,
            m_stats.copyMilliseconds,
            m_stats.peakLoadPages, m_stats.peakLoadApplyMs, m_stats.peakLoadCopyMs);
        return buffer;
    }
}
