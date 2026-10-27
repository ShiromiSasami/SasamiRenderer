#include "Renderer/Scene/DynamicVertexRing.h"

#include <algorithm>

namespace SasamiEngine::Render
{
    bool DynamicVertexRing::Create(GraphicsDevice& device, const Vertex* vertices, size_t vertexCount, uint32_t slotCount)
    {
        Release(device);
        if (!vertices || vertexCount == 0 || slotCount < 2) {
            return false;
        }
        RhiBufferDesc desc{};
        desc.sizeInBytes = static_cast<uint64_t>(sizeof(Vertex)) * vertexCount;
        desc.strideInBytes = sizeof(Vertex);
        desc.usage = RhiBufferUsageFlags::Vertex;
        desc.memoryUsage = RhiMemoryUsage::CpuToGpu;
        desc.initialState = RhiResourceState::Common;
        m_slots.resize(slotCount);
        for (Slot& slot : m_slots) {
            slot.buffer = device.CreateRhiBuffer(desc, vertices);
            if (!slot.buffer.IsValid()) {
                Release(device);
                return false;
            }
        }
        m_vertexCount = vertexCount;
        m_current = 0;
        m_written = false;
        return true;
    }

    void DynamicVertexRing::Release(GraphicsDevice& device)
    {
        for (Slot& slot : m_slots) {
            if (slot.buffer.IsValid()) {
                device.DestroyRhiResource(slot.buffer);
            }
        }
        m_slots.clear();
        m_current = 0;
        m_vertexCount = 0;
        m_lastWrittenVertices = 0;
        m_written = false;
    }

    bool DynamicVertexRing::Write(GraphicsDevice& device, const Vertex* vertices, size_t vertexCount,
                                  size_t first, size_t count, uint64_t frame)
    {
        if (m_slots.empty() || !vertices || vertexCount != m_vertexCount || first > m_vertexCount ||
            count > m_vertexCount - first) {
            return false;
        }
        if (count > 0) {
            const size_t end = first + count;
            for (Slot& slot : m_slots) {
                if (slot.pendingBegin == slot.pendingEnd) {
                    slot.pendingBegin = first;
                    slot.pendingEnd = end;
                } else {
                    slot.pendingBegin = (std::min)(slot.pendingBegin, first);
                    slot.pendingEnd = (std::max)(slot.pendingEnd, end);
                }
            }
        }
        const bool sameFrame = m_written && frame == m_lastWriteFrame;
        const uint32_t next = sameFrame ? m_current : (m_current + 1) % static_cast<uint32_t>(m_slots.size());
        Slot& target = m_slots[next];
        const size_t written = target.pendingEnd - target.pendingBegin;
        if (written > 0 &&
            !device.UpdateRhiBuffer(target.buffer, sizeof(Vertex) * target.pendingBegin,
                                    vertices + target.pendingBegin, sizeof(Vertex) * written)) {
            return false;
        }
        target.pendingBegin = 0;
        target.pendingEnd = 0;
        m_current = next;
        m_lastWrittenVertices = written;
        m_lastWriteFrame = frame;
        m_written = true;
        return true;
    }
}
