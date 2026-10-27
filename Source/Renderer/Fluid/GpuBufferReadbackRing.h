#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>

namespace SasamiEngine::Render
{
    // Non-blocking copy of one GPU buffer to the CPU, a few frames late. Record() appends a
    // CopyBufferRegion into the caller's (frame) command list, into slot `records % SLOT_COUNT` of a ring of
    // persistently mapped readback-heap buffers; Latest() returns the newest slot recorded READ_LAG or more
    // records ago, assuming the GPU has finished a frame READ_LAG frames later (the same assumption as
    // GpuTimestampProfiler::kFrameLatency and Voxel::GpuWindParticleStream; not checked against a fence).
    // Record once per frame at most. D3D12 only. Release() must run when the GPU is idle.
    class GpuBufferReadbackRing
    {
    public:
        static constexpr uint32_t SLOT_COUNT = 4;
        static constexpr uint32_t READ_LAG = 3;

        ~GpuBufferReadbackRing();

        // (Re)creates the ring for `bytes` per slot if needed and resets the record count. False on failure.
        bool Ensure(GraphicsDevice& device, uint64_t bytes);
        void Release();
        bool IsReady() const { return m_bytes != 0; }
        uint64_t Bytes() const { return m_bytes; }

        // `source` must be in `sourceState`; it goes to COPY_SOURCE and back around the copy.
        bool Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState);

        // The newest readable copy, or nullptr when fewer than READ_LAG + 1 records exist.
        // outRecord: its record number (0-based), so callers can skip one they have already consumed.
        const void* Latest(uint64_t& outRecord) const;
        uint64_t RecordCount() const { return m_records; }

    private:
        Resource m_slots[SLOT_COUNT];
        void* m_mapped[SLOT_COUNT] = {};
        uint64_t m_bytes = 0;
        uint64_t m_records = 0;
    };
}

namespace SasamiEngine { using namespace Render; }
