#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    struct ClusterSelectParams
    {
        const float* viewProjection = nullptr;  // row-major float[16]
        float cameraPos[3] = {0, 0, 0};
        float lodThresholdPx = 1.0f;
        float projScale = 0.0f;
        float zNear = 0.05f;
        bool  orthographic = false;
        uint32_t instanceCount = 0;
        uint32_t groupCount = 0;      // groups in the scene: sizes the wanted-bitset reset/readback (0 = all kMaxGroups)
        D3D12_GPU_VIRTUAL_ADDRESS pageOffsetTableVA = 0;  // uint per group: page byte offset in the pool or 0xFFFFFFFF when not resident
        D3D12_GPU_VIRTUAL_ADDRESS pagePoolVA = 0;
        bool collectStats = false;  // opt in to the traversal counters (nodes/levels/examined); off by default so the steady-state path runs no extra atomics
    };

    struct ClusterFeedbackReadout
    {
        uint32_t visibleCount = 0;        // counter byte 0, clamped to the view's capacity by CS_ClusterSelectArgs
        uint32_t overflow = 0;            // counter byte 4
        uint32_t instancesProcessed = 0;  // counter byte 8
        uint32_t nodesVisited = 0;        // counter byte 12: BVH nodes visited by the traversal
        uint32_t maxTraversalLevels = 0;  // counter byte 16: deepest per-instance level count (barrier round-trips)
        uint32_t clustersExamined = 0;    // counter byte 20: cluster descs loaded from the page pool at leaves
        uint32_t requestedCount = 0;      // counter byte 24: appends the traversal attempted, before the capacity clamp
        uint32_t visibleCapacity = 0;     // entries the slot's visible list held when this view was recorded (CPU side)
        std::vector<uint32_t> wantedBits; // kWantedWordCount words (resized by ReadFeedback)
    };

    // GPU-driven cluster selection (BVH traversal + LOD test) feeding an indirect mesh-shader dispatch.
    class ClusterGpuCulling
    {
    public:
        // Visible-list capacity is per view slot and follows that slot's own measured demand; it is
        // not one constant shared by every view, because the demand per view is nothing like
        // uniform. Measured, Bistro (1697 instances, 4 shadow cascades, render.streamstats and the
        // GrowVisibleBuffer log at the saved camera): the four cascades want 7507 / 3887 / 3641 /
        // 148 entries -- a 50x spread, with the heaviest cascade alone accounting for half of the
        // 15183 total. Sizing every view for the heaviest would hold 4 x 7507 = 30028 entries to
        // serve 15183, so each slot grows on its own instead.
        //
        // kInitialVisibleClusters is deliberately the value the fixed ceiling used to have, so a
        // Bistro-sized scene never grows and its footprint is byte-identical to the pre-growth code.
        // kVisibleClusterCeiling is the hard stop on growth: at the heaviest cascade's 4.4 clusters
        // per instance, 100k instances land near 440k entries, and the pessimistic
        // 18-clusters-per-instance reading of the same scene lands near 1.8M, so the ceiling is set
        // above that with headroom. Reaching it does not corrupt anything: CS_ClusterSelect raises
        // kOverflowVisibleList (0x8) and render.streamstats reports it, the same as before.
        static constexpr uint32_t kInitialVisibleClusters = 1u << 18;   // 262,144 entries = 2 MB of uint2 per view slot
        static constexpr uint32_t kVisibleClusterCeiling  = 1u << 22;   // 4,194,304 entries = 32 MB, only if a view ever demands it
        static constexpr uint32_t kViewSlotsPerFrame  = 6;          // 4 shadow cascades + main view + spare
        static constexpr uint32_t kMainViewSlot       = 4;          // the camera's GBuffer view (slots 0..3 are the cascades)
        static constexpr uint32_t kMaxGroups          = 1u << 20;   // bitset domain
        static constexpr uint32_t kWantedWordCount    = kMaxGroups / 32u;  // 128 KB per slot

        ~ClusterGpuCulling();

        bool Initialize(GraphicsDevice& device, uint32_t frameCount);
        void Release();
        bool IsValid() const;

        // Records for one view: counter/args reset (CopyBufferRegion from a zero buffer), CS_ClusterSelect
        // (one thread group per instance, dispatched 2D at kSelectGroupsX wide), CS_ClusterSelectArgs, and barriers leaving args in INDIRECT_ARGUMENT and the
        // visible list + counter in NON_PIXEL_SHADER_RESOURCE. The CB is uploaded into this slot's mapped CB.
        //
        // Also sizes this slot's visible list before recording into it: the completed readback of
        // this same slot carries the pre-clamp demand of its last run, and the buffer is grown (never
        // shrunk, like ClusterInstanceBuffer) to cover it. The growth therefore lags the demand by
        // frameCount frames -- the usual latency of this feedback -- and the frames in between report
        // kOverflowVisibleList instead of dropping clusters silently. GetVisibleListVA must be read
        // after this call, since a growth replaces the buffer.
        bool RecordSelect(CommandList& cmdList, uint32_t frameIndex, uint32_t viewSlot, const ClusterSelectParams& params,
                          D3D12_GPU_VIRTUAL_ADDRESS clustersVA, D3D12_GPU_VIRTUAL_ADDRESS groupsVA,
                          D3D12_GPU_VIRTUAL_ADDRESS nodesVA, D3D12_GPU_VIRTUAL_ADDRESS instancesVA);

        // After the indirect draw consumed the slot: transition visible/counter/args back to UNORDERED_ACCESS.
        void RecordRelease(CommandList& cmdList, uint32_t frameIndex, uint32_t viewSlot);

        D3D12_GPU_VIRTUAL_ADDRESS GetVisibleListVA(uint32_t frameIndex, uint32_t viewSlot) const;
        D3D12_GPU_VIRTUAL_ADDRESS GetCounterVA(uint32_t frameIndex, uint32_t viewSlot) const;
        ID3D12Resource* GetArgsResource(uint32_t frameIndex, uint32_t viewSlot) const;  // 12-byte DispatchMesh args at offset 0
        ID3D12CommandSignature* GetDispatchMeshCommandSignature() const;

        // Maps the slot's readback buffer and copies out the counter and the wanted bitset. Only valid for a
        // (frameIndex, viewSlot) whose GPU work has completed — the renderer calls it at the start of the frame
        // that reuses frameIndex, before RecordSelect. Returns false if the slot has never been recorded.
        bool ReadFeedback(uint32_t frameIndex, uint32_t viewSlot, ClusterFeedbackReadout& out) const;

    private:
        // Per-(frame, view) GPU resources. Buffers are created directly in UNORDERED_ACCESS
        // (GpuOnly, no initial data) so no CopyDest transition is needed at creation time; the
        // *State fields track what RecordSelect/RecordRelease left the resource in.
        struct ViewSlotResources
        {
            RhiBufferHandle visibleHandle{};
            Resource* visibleCompat = nullptr;
            D3D12_RESOURCE_STATES visibleState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            uint32_t visibleCapacity = 0;          // entries the current visible buffer holds
            uint32_t recordedVisibleCapacity = 0;  // capacity in force for the last RecordSelect, i.e. the one the pending readback belongs to

            RhiBufferHandle counterHandle{};
            Resource* counterCompat = nullptr;
            D3D12_RESOURCE_STATES counterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            RhiBufferHandle argsHandle{};
            Resource* argsCompat = nullptr;
            D3D12_RESOURCE_STATES argsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            RhiBufferHandle cbHandle{};
            Resource* cbCompat = nullptr;
            void* cbMapped = nullptr;

            RhiBufferHandle wantedHandle{};
            Resource* wantedCompat = nullptr;
            D3D12_RESOURCE_STATES wantedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            RhiBufferHandle readbackHandle{};
            Resource* readbackCompat = nullptr;

            bool recorded = false;
            uint32_t wantedBytes = 0;     // bytes of the wanted bitset reset/copied for the last RecordSelect

            // Visible buffers displaced by a growth of this slot, kept alive until no in-flight
            // frame can still reference them. Same scheme (and the same reasoning) as
            // ClusterInstanceBuffer::RetiredBuffer: the frame coordinator has already waited on
            // this slot's fence before RecordSelect runs, so the GPU is done with the old buffer,
            // but holding it for frameCount further recordings of this slot -- one per frame, since
            // a (frameIndex, viewSlot) is recorded at most once per frame -- removes any dependence
            // on that ordering. Per slot rather than per object precisely so that the countdown
            // ticks once per frame instead of once per view.
            struct RetiredVisibleBuffer
            {
                RhiBufferHandle handle{};
                uint32_t updatesRemaining = 0;
            };
            std::vector<RetiredVisibleBuffer> retiredVisible;
        };

        bool CreateZeroBuffer();
        bool CreateRootSignatureAndPsos();
        bool CreateCommandSignature();
        bool CreateSlotResources(ViewSlotResources& slot);
        void ReleaseSlot(ViewSlotResources& slot);

        // Visible-list capacity management, all driven from the slot's own feedback.
        bool CreateVisibleBuffer(uint32_t capacity, RhiBufferHandle& outHandle, Resource*& outCompat) const;
        uint32_t ReadRequestedVisibleCount(const ViewSlotResources& slot) const;  // counter byte 24 of the completed readback; 0 when never recorded
        void GrowVisibleBuffer(ViewSlotResources& slot, uint32_t required);       // no-op when it already fits; clamps to kVisibleClusterCeiling
        void TickRetiredVisible(ViewSlotResources& slot);
        const ViewSlotResources* FindSlot(uint32_t frameIndex, uint32_t viewSlot) const;
        ViewSlotResources* FindSlot(uint32_t frameIndex, uint32_t viewSlot);

        GraphicsDevice* m_device = nullptr;
        uint32_t m_frameCount = 0;
        bool m_valid = false;

        std::vector<ViewSlotResources> m_slots;

        RhiBufferHandle m_zeroBufferHandle{};
        Resource* m_zeroBufferCompat = nullptr;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_selectPso;
        ComPtr<ID3D12PipelineState> m_argsPso;
        ComPtr<ID3D12CommandSignature> m_commandSignature;
    };
}

namespace SasamiEngine { using namespace Render; }
