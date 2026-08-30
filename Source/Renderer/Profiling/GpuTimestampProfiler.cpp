#include "Renderer/Profiling/GpuTimestampProfiler.h"

#include "Renderer/Resources/RenderPipelineStateCacheLog.h"

namespace SasamiRenderer
{
    bool GpuTimestampProfiler::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        Shutdown();

        if (!device || !queue) {
            LogFail("GpuTimestampProfiler::Initialize (null device/queue)", E_INVALIDARG);
            return false;
        }

        const uint32_t totalSlots = kMaxScopesPerFrame * 2u * kFrameLatency;

        D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
        queryHeapDesc.Type     = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count    = totalSlots;
        queryHeapDesc.NodeMask = 0;
        HRESULT hr = device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&m_queryHeap));
        if (FAILED(hr)) {
            LogFail("GpuTimestampProfiler::CreateQueryHeap", hr);
            Shutdown();
            return false;
        }

        D3D12_HEAP_PROPERTIES readbackHeapProps = {};
        readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC readbackDesc = {};
        readbackDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Width              = sizeof(uint64_t) * static_cast<uint64_t>(totalSlots);
        readbackDesc.Height             = 1;
        readbackDesc.DepthOrArraySize   = 1;
        readbackDesc.MipLevels          = 1;
        readbackDesc.Format             = DXGI_FORMAT_UNKNOWN;
        readbackDesc.SampleDesc.Count   = 1;
        readbackDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readbackDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        hr = device->CreateCommittedResource(&readbackHeapProps,
                                              D3D12_HEAP_FLAG_NONE,
                                              &readbackDesc,
                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr,
                                              IID_PPV_ARGS(&m_readbackBuffer));
        if (FAILED(hr)) {
            LogFail("GpuTimestampProfiler::CreateCommittedResource (readback)", hr);
            Shutdown();
            return false;
        }

        hr = queue->GetTimestampFrequency(&m_timestampFrequency);
        if (FAILED(hr) || m_timestampFrequency == 0) {
            LogFail("GpuTimestampProfiler::GetTimestampFrequency", hr);
            Shutdown();
            return false;
        }

        // Map once and keep it open for the lifetime of the profiler; nothing has been resolved
        // into the buffer yet, so the initial read range is empty. Per-frame reads afterward walk
        // the persistently mapped pointer directly instead of Map/Unmap-ing every frame.
        D3D12_RANGE emptyReadRange = { 0, 0 };
        void* mapped = nullptr;
        hr = m_readbackBuffer->Map(0, &emptyReadRange, &mapped);
        if (FAILED(hr)) {
            LogFail("GpuTimestampProfiler::Map (readback)", hr);
            Shutdown();
            return false;
        }
        m_mappedReadback = static_cast<uint8_t*>(mapped);

        for (uint32_t ring = 0; ring < kFrameLatency; ++ring) {
            m_frameSlots[ring].appFrameIndex = 0;
            m_frameSlots[ring].scopeCount    = 0;
            m_frameSlots[ring].valid         = false;
            m_frameSlots[ring].scopeNames.assign(kMaxScopesPerFrame, std::string());
        }

        m_currentRing      = 0;
        m_currentFrameBase = 0;
        m_scopesThisFrame  = 0;
        m_openScopeIndex   = -1;
        m_overflowLogged   = false;
        m_results.clear();

        m_ready = true;
        return true;
    }

    void GpuTimestampProfiler::Shutdown()
    {
        if (m_readbackBuffer && m_mappedReadback) {
            D3D12_RANGE emptyWrittenRange = { 0, 0 };
            m_readbackBuffer->Unmap(0, &emptyWrittenRange);
        }
        m_mappedReadback = nullptr;
        m_readbackBuffer.Reset();
        m_queryHeap.Reset();

        m_timestampFrequency = 0;
        m_currentRing         = 0;
        m_currentFrameBase    = 0;
        m_scopesThisFrame     = 0;
        m_openScopeIndex      = -1;
        m_overflowLogged      = false;

        for (uint32_t ring = 0; ring < kFrameLatency; ++ring) {
            m_frameSlots[ring].appFrameIndex = 0;
            m_frameSlots[ring].scopeCount    = 0;
            m_frameSlots[ring].valid         = false;
            m_frameSlots[ring].scopeNames.clear();
        }

        m_results.clear();
        m_ready = false;
    }

    void GpuTimestampProfiler::BeginFrame(uint64_t frameIndex)
    {
        if (!m_ready) {
            return;
        }

        m_currentRing      = GetRingSlot(frameIndex);
        m_currentFrameBase = m_currentRing * kMaxScopesPerFrame * 2u;
        m_scopesThisFrame  = 0;
        m_openScopeIndex   = -1;

        FrameSlot& slot = m_frameSlots[m_currentRing];
        slot.appFrameIndex = frameIndex;
        slot.scopeCount    = 0;
        slot.valid         = false;
    }

    void GpuTimestampProfiler::BeginScope(ID3D12GraphicsCommandList* cmdList, std::string_view name)
    {
        if (!m_ready || !cmdList) {
            return;
        }

        // Nesting is unsupported; a stray Begin while one is already open would corrupt the
        // slot indexing, so just ignore it rather than let it clobber another scope's result.
        if (m_openScopeIndex >= 0) {
            return;
        }

        if (m_scopesThisFrame >= kMaxScopesPerFrame) {
            if (!m_overflowLogged) {
                LogFail("GpuTimestampProfiler::BeginScope (kMaxScopesPerFrame exceeded)", E_FAIL);
                m_overflowLogged = true;
            }
            return;
        }

        const uint32_t index = m_scopesThisFrame;
        m_openScopeIndex = static_cast<int32_t>(index);

        FrameSlot& slot = m_frameSlots[m_currentRing];
        slot.scopeNames[index].assign(name);

        cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_currentFrameBase + index * 2u);
    }

    void GpuTimestampProfiler::EndScope(ID3D12GraphicsCommandList* cmdList)
    {
        if (!m_ready || !cmdList) {
            return;
        }

        // Guard against an End with no matching Begin (e.g. mismatched call counts) so we never
        // write into a slot that wasn't reserved by BeginScope.
        if (m_openScopeIndex < 0) {
            return;
        }

        const uint32_t index = static_cast<uint32_t>(m_openScopeIndex);
        cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_currentFrameBase + index * 2u + 1u);

        m_openScopeIndex = -1;
        ++m_scopesThisFrame;
    }

    void GpuTimestampProfiler::EndFrame(ID3D12GraphicsCommandList* cmdList)
    {
        if (!m_ready || !cmdList) {
            return;
        }

        // A dangling Begin without a matching End never wrote its end-query, so its slot holds
        // no valid pair; drop it rather than resolve garbage.
        m_openScopeIndex = -1;

        FrameSlot& slot = m_frameSlots[m_currentRing];
        slot.scopeCount = m_scopesThisFrame;
        m_lastResolvedScopeCount = m_scopesThisFrame;

        if (slot.scopeCount > 0) {
            const UINT64 destOffsetBytes = sizeof(uint64_t) * static_cast<UINT64>(m_currentFrameBase);
            cmdList->ResolveQueryData(m_queryHeap.Get(),
                                       D3D12_QUERY_TYPE_TIMESTAMP,
                                       m_currentFrameBase,
                                       slot.scopeCount * 2u,
                                       m_readbackBuffer.Get(),
                                       destOffsetBytes);
        }

        // Only after the resolve has been recorded is this ring slot's data considered on its
        // way to becoming readable once the GPU actually finishes executing this frame.
        slot.valid = true;
    }

    void GpuTimestampProfiler::UpdateResults(uint64_t completedFrameIndex)
    {
        if (!m_ready || !m_mappedReadback) {
            return;
        }

        const uint32_t ring = GetRingSlot(completedFrameIndex);
        const FrameSlot& slot = m_frameSlots[ring];

        m_lastUpdateRequestedFrame = completedFrameIndex;
        m_lastUpdateSlotFrame      = slot.appFrameIndex;
        m_lastUpdateSlotValid      = slot.valid;

        // The caller guarantees completedFrameIndex's GPU work is done, but the ring slot might
        // never have been submitted (valid == false) or might since have been overwritten by a
        // later frame that reused the same ring slot (appFrameIndex mismatch). Either case means
        // reading the mapped memory now would return stale or torn data, so bail and keep
        // whatever results were already published.
        if (!slot.valid || slot.appFrameIndex != completedFrameIndex) {
            return;
        }

        const auto* timestamps = reinterpret_cast<const uint64_t*>(m_mappedReadback);
        const uint32_t frameBase = ring * kMaxScopesPerFrame * 2u;

        std::vector<ScopeResult> results;
        results.reserve(slot.scopeCount);
        for (uint32_t i = 0; i < slot.scopeCount; ++i) {
            const uint64_t begin = timestamps[frameBase + i * 2u];
            const uint64_t end   = timestamps[frameBase + i * 2u + 1u];

            ScopeResult result;
            result.name = slot.scopeNames[i];
            result.milliseconds = (end > begin && m_timestampFrequency > 0)
                                       ? static_cast<double>(end - begin) * 1000.0 / static_cast<double>(m_timestampFrequency)
                                       : 0.0;
            results.push_back(std::move(result));
        }

        m_results = std::move(results);
    }
}
