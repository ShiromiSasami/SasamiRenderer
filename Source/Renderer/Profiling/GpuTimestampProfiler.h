#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

namespace SasamiRenderer
{
    // Per-pass GPU timing via D3D12 timestamp queries.
    //
    // Timestamps are written on the GPU timeline, so the results for frame N can only be read
    // back after the GPU has finished frame N. This class therefore keeps kFrameLatency frames
    // of query slots in flight and reports the most recent frame whose readback is known safe;
    // callers get numbers that are a couple of frames old, which is exactly what a profiler
    // display wants and must not be mistaken for the current frame.
    class GpuTimestampProfiler
    {
    public:
        static constexpr uint32_t kMaxScopesPerFrame = 64u;
        static constexpr uint32_t kFrameLatency      = 3u;

        struct ScopeResult
        {
            std::string name;
            double      milliseconds = 0.0;
        };

        bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
        void Shutdown();
        bool IsReady() const { return m_ready; }

        // Call once at the top of a frame, before any BeginScope.
        // 64-bit to match FrameSlot::appFrameIndex and UpdateResults: a narrower counter here
        // would store a truncated index that UpdateResults' full-width argument never matches.
        void BeginFrame(uint64_t frameIndex);

        // Bracket GPU work. Nesting is NOT supported: each Begin must be matched by the next End.
        void BeginScope(ID3D12GraphicsCommandList* cmdList, std::string_view name);
        void EndScope(ID3D12GraphicsCommandList* cmdList);

        // Call after the last scope of the frame; records the ResolveQueryData.
        void EndFrame(ID3D12GraphicsCommandList* cmdList);

        // Results for the newest frame whose GPU work is known complete. Empty until enough
        // frames have elapsed. completedFenceValue is the caller's frame fence progress.
        const std::vector<ScopeResult>& GetResults() const { return m_results; }

        // Diagnostics for when GetResults() stays empty: separates "no scope was ever opened"
        // (scopes-this-frame stays 0 -> the graph is not calling BeginScope) from "scopes were
        // recorded but never read back" (last-resolved is non-zero while results stay empty).
        uint32_t GetScopesThisFrame() const { return m_scopesThisFrame; }
        uint32_t GetLastResolvedScopeCount() const { return m_lastResolvedScopeCount; }
        // What the last UpdateResults call actually saw, so a rejected read can be told apart
        // from one that was never attempted, and an index mismatch from an unsubmitted slot.
        uint64_t GetLastUpdateRequestedFrame() const { return m_lastUpdateRequestedFrame; }
        uint64_t GetLastUpdateSlotFrame() const { return m_lastUpdateSlotFrame; }
        bool     GetLastUpdateSlotValid() const { return m_lastUpdateSlotValid; }
        void UpdateResults(uint64_t completedFrameIndex);

    private:
        // Bookkeeping for one ring-buffer slot's worth of queries (kMaxScopesPerFrame scopes).
        // appFrameIndex/valid let UpdateResults reject a slot that either hasn't been submitted
        // yet or has since been overwritten by a different frame than the one being asked for.
        struct FrameSlot
        {
            uint64_t                 appFrameIndex = 0;
            uint32_t                 scopeCount    = 0;
            bool                     valid         = false;
            std::vector<std::string> scopeNames;
        };

        uint32_t GetRingSlot(uint64_t frameIndex) const { return static_cast<uint32_t>(frameIndex % kFrameLatency); }

        bool     m_ready               = false;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource>  m_readbackBuffer;
        uint8_t* m_mappedReadback      = nullptr;
        uint64_t m_timestampFrequency  = 0;

        uint32_t m_currentRing         = 0;
        uint32_t m_currentFrameBase    = 0; // query-slot index of the first begin-query of the current frame
        uint32_t m_scopesThisFrame     = 0;
        int32_t  m_openScopeIndex      = -1; // index into the current frame's scopes for the pending Begin, -1 if none open
        bool     m_overflowLogged      = false;
        uint32_t m_lastResolvedScopeCount = 0;
        uint64_t m_lastUpdateRequestedFrame = 0;
        uint64_t m_lastUpdateSlotFrame      = 0;
        bool     m_lastUpdateSlotValid      = false;

        FrameSlot m_frameSlots[kFrameLatency];

        std::vector<ScopeResult> m_results;
    };
}
