#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    // One persistently mapped upload buffer per frame slot, each grown on demand to fit what the
    // frame is about to write. Element-type agnostic: the owner supplies the element stride and
    // writes through GetMapped.
    //
    // Growth doubles from the slot's current capacity up to maxElements. Only the slot about to be
    // written is replaced -- the frame coordinator has already waited on its fence -- and the
    // displaced buffer is still held in a retirement list for frameCount further TickRetired calls,
    // so every slot has been recycled before it is released and nothing depends on that ordering.
    //
    // Extracted from ClusterInstanceBuffer so the per-instance cluster material table can grow with
    // the same rules instead of a second copy of them.
    class PerFrameGrowableUploadBuffer
    {
    public:
        ~PerFrameGrowableUploadBuffer();

        // debugName prefixes the growth log lines (e.g. "ClusterInstanceBuffer").
        bool Initialize(GraphicsDevice& device, uint32_t frameCount, uint32_t initialCapacity,
                        uint32_t elementStride, uint32_t maxElements, const char* debugName);
        void Release();
        bool IsValid() const;

        // Ages the retirement list by one update; call once per Update of the owner, before EnsureCapacity.
        void TickRetired();

        // Grows slot frameIndex so it holds at least `required` elements. Returns false (slot left
        // unchanged and still usable at its old capacity) when the allocation fails or `required`
        // exceeds maxElements.
        bool EnsureCapacity(uint32_t frameIndex, uint32_t required);

        uint8_t* GetMapped(uint32_t frameIndex) const;          // nullptr for an invalid slot
        uint32_t GetSlotCapacity(uint32_t frameIndex) const;    // in elements; 0 for an invalid slot
        D3D12_GPU_VIRTUAL_ADDRESS GetGpuVA(uint32_t frameIndex) const;

        uint32_t GetFrameCount() const { return m_frameCount; }
        uint32_t GetElementStride() const { return m_elementStride; }
        uint32_t GetMaxElements() const { return m_maxElements; }

    private:
        struct Slot
        {
            Resource buffer;
            uint8_t* mapped   = nullptr;   // persistently mapped for the lifetime of the buffer
            uint32_t capacity = 0;
        };

        // A buffer displaced by a growth, kept alive until no in-flight frame can still reference it.
        struct RetiredBuffer
        {
            Resource buffer;
            uint32_t updatesRemaining = 0;
        };

        bool CreateSlotBuffer(uint32_t capacity, Slot& outSlot) const;
        double MegabytesFor(uint32_t elements) const;

        GraphicsDevice* m_device = nullptr;
        uint32_t m_frameCount      = 0;
        uint32_t m_initialCapacity = 0;
        uint32_t m_elementStride   = 0;
        uint32_t m_maxElements     = 0;
        const char* m_debugName    = "";

        std::vector<Slot> m_slots;
        std::vector<RetiredBuffer> m_retired;
    };
}

namespace SasamiEngine { using namespace Render; }
