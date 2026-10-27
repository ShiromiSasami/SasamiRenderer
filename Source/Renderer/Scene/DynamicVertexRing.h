#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::Render
{
    // Several CpuToGpu copies of one mesh's vertex buffer, written round-robin so the CPU never rewrites the
    // copy that a frame still in flight reads (one buffer rewritten in place every frame raced the GPU).
    // Each Write moves to the next copy. A copy misses the ranges written while it was not current, so
    // every copy keeps a pending range (the hull of those writes) and rewrites it from the caller's full
    // array on its turn; after its write a copy equals that array.
    // A second Write within the same frame (same `frame` value) rewrites the current copy instead of moving on:
    // that copy became current during this frame, so no submitted frame reads it, while moving twice could reach
    // a copy that one still does. Contract: more copies than frames the GPU can be behind.
    class DynamicVertexRing
    {
    public:
        // Creates `slotCount` copies (>= 2), each filled with `vertices`.
        bool Create(GraphicsDevice& device, const Vertex* vertices, size_t vertexCount, uint32_t slotCount);
        // The GPU must no longer read any copy.
        void Release(GraphicsDevice& device);

        // Makes the next copy current (or keeps the current one when `frame` equals the last Write's) after
        // writing [first, first + count) plus that copy's pending range from `vertices`, which holds all
        // VertexCount() vertices. count may be 0. On failure the current copy stays as it was.
        bool Write(GraphicsDevice& device, const Vertex* vertices, size_t vertexCount, size_t first, size_t count,
                   uint64_t frame);

        RhiBufferHandle Current() const { return m_slots.empty() ? RhiBufferHandle{} : m_slots[m_current].buffer; }
        size_t VertexCount() const { return m_vertexCount; }
        uint32_t SlotCount() const { return static_cast<uint32_t>(m_slots.size()); }
        // Vertices the last successful Write copied (its range merged with the copy's pending range).
        size_t LastWrittenVertices() const { return m_lastWrittenVertices; }

    private:
        struct Slot
        {
            RhiBufferHandle buffer{};
            size_t pendingBegin = 0;   // empty when pendingBegin == pendingEnd
            size_t pendingEnd = 0;
        };

        std::vector<Slot> m_slots;
        uint32_t m_current = 0;
        size_t m_vertexCount = 0;
        size_t m_lastWrittenVertices = 0;
        uint64_t m_lastWriteFrame = 0;
        bool m_written = false;   // m_lastWriteFrame is valid
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
