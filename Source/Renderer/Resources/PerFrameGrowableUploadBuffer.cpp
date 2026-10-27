#define NOMINMAX
#include "Renderer/Resources/PerFrameGrowableUploadBuffer.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"

#include <cstdio>

namespace SasamiEngine::Render
{
    PerFrameGrowableUploadBuffer::~PerFrameGrowableUploadBuffer()
    {
        Release();
    }

    bool PerFrameGrowableUploadBuffer::Initialize(GraphicsDevice& device, uint32_t frameCount, uint32_t initialCapacity,
                                                  uint32_t elementStride, uint32_t maxElements, const char* debugName)
    {
        Release();

        if (frameCount == 0 || initialCapacity == 0 || elementStride == 0 || maxElements == 0 ||
            initialCapacity > maxElements)
        {
            return false;
        }

        m_device          = &device;
        m_initialCapacity = initialCapacity;
        m_elementStride   = elementStride;
        m_maxElements     = maxElements;
        m_debugName       = debugName != nullptr ? debugName : "";
        m_slots.resize(frameCount);

        for (uint32_t i = 0; i < frameCount; ++i)
        {
            if (!CreateSlotBuffer(initialCapacity, m_slots[i]))
            {
                Release();
                return false;
            }
        }

        m_frameCount = frameCount;
        return true;
    }

    void PerFrameGrowableUploadBuffer::Release()
    {
        m_slots.clear();
        m_retired.clear();

        m_device          = nullptr;
        m_frameCount      = 0;
        m_initialCapacity = 0;
        m_elementStride   = 0;
        m_maxElements     = 0;
        m_debugName       = "";
    }

    bool PerFrameGrowableUploadBuffer::IsValid() const
    {
        return m_frameCount > 0 && m_slots.size() == m_frameCount && m_device != nullptr;
    }

    bool PerFrameGrowableUploadBuffer::CreateSlotBuffer(uint32_t capacity, Slot& outSlot) const
    {
        if (!m_device || capacity == 0)
        {
            return false;
        }

        const uint64_t bytes = static_cast<uint64_t>(capacity) * m_elementStride;

        Slot created{};
        void* mapped = nullptr;
        if (!ResourceUploadUtility::CreateUploadBuffer(*m_device, bytes, created.buffer, &mapped))
        {
            return false;
        }

        created.mapped   = static_cast<uint8_t*>(mapped);
        created.capacity = capacity;
        outSlot          = created;
        return true;
    }

    double PerFrameGrowableUploadBuffer::MegabytesFor(uint32_t elements) const
    {
        return static_cast<double>(static_cast<uint64_t>(elements) * m_elementStride) / (1024.0 * 1024.0);
    }

    bool PerFrameGrowableUploadBuffer::EnsureCapacity(uint32_t frameIndex, uint32_t required)
    {
        if (!IsValid() || frameIndex >= m_slots.size())
        {
            return false;
        }

        Slot& slot = m_slots[frameIndex];
        if (required <= slot.capacity)
        {
            return true;
        }
        if (slot.capacity >= m_maxElements)
        {
            return false;
        }

        // Doubling, so a scene that grows by one element per frame does not reallocate every frame.
        uint32_t target = slot.capacity != 0 ? slot.capacity : m_initialCapacity;
        while (target < required)
        {
            if (target >= m_maxElements)
            {
                return false;
            }
            target = (target > m_maxElements / 2u) ? m_maxElements : target * 2u;
        }

        Slot grown{};
        if (!CreateSlotBuffer(target, grown))
        {
            char buffer[192] = {};
            std::snprintf(buffer, sizeof(buffer),
                          "[%s] GrowSlot: allocation for %u elements (%.1f MB) failed; keeping the %u-element buffer.\n",
                          m_debugName, target, MegabytesFor(target), slot.capacity);
            DebugLog(buffer);
            return false;
        }

        // The displaced buffer is not freed here. The frame coordinator has already waited on this
        // slot's fence, so the GPU is done with it, but holding it for frameCount further updates --
        // long enough for every slot to be recycled -- removes any dependence on that ordering.
        if (slot.buffer.IsValid())
        {
            RetiredBuffer retired{};
            retired.buffer           = slot.buffer;
            retired.updatesRemaining = m_frameCount;
            m_retired.push_back(retired);
        }

        char buffer[160] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "[%s] GrowSlot: %u -> %u elements (%.1f MB per frame slot).\n",
                      m_debugName, slot.capacity, target, MegabytesFor(target));
        DebugLog(buffer);

        slot = grown;
        return true;
    }

    void PerFrameGrowableUploadBuffer::TickRetired()
    {
        for (size_t i = m_retired.size(); i-- > 0;)
        {
            if (m_retired[i].updatesRemaining > 0)
            {
                --m_retired[i].updatesRemaining;
            }
            if (m_retired[i].updatesRemaining == 0)
            {
                m_retired.erase(m_retired.begin() + static_cast<ptrdiff_t>(i));
            }
        }
    }

    uint8_t* PerFrameGrowableUploadBuffer::GetMapped(uint32_t frameIndex) const
    {
        if (!IsValid() || frameIndex >= m_slots.size())
        {
            return nullptr;
        }
        return m_slots[frameIndex].mapped;
    }

    uint32_t PerFrameGrowableUploadBuffer::GetSlotCapacity(uint32_t frameIndex) const
    {
        if (!IsValid() || frameIndex >= m_slots.size())
        {
            return 0;
        }
        return m_slots[frameIndex].capacity;
    }

    D3D12_GPU_VIRTUAL_ADDRESS PerFrameGrowableUploadBuffer::GetGpuVA(uint32_t frameIndex) const
    {
        if (!IsValid() || frameIndex >= m_slots.size() || !m_slots[frameIndex].buffer.IsValid())
        {
            return 0;
        }
        return m_slots[frameIndex].buffer.GetGPUVirtualAddress();
    }
}
