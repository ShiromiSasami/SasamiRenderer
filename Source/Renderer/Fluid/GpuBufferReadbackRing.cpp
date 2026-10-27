#include "Renderer/Fluid/GpuBufferReadbackRing.h"

#include "d3dx12.h"

namespace SasamiEngine::Render
{
    GpuBufferReadbackRing::~GpuBufferReadbackRing()
    {
        Release();
    }

    bool GpuBufferReadbackRing::Ensure(GraphicsDevice& device, uint64_t bytes)
    {
        if (bytes == 0) {
            return false;
        }
        if (m_bytes == bytes) {
            return true;
        }
        Release();

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_slots[i]))) {
                Release();
                return false;
            }
            // Readback buffers may stay mapped for their lifetime (D3D12 docs, ID3D12Resource::Map).
            const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(bytes) };
            if (FAILED(m_slots[i]->Map(0, &readRange, &m_mapped[i])) || !m_mapped[i]) {
                Release();
                return false;
            }
        }
        m_bytes = bytes;
        m_records = 0;
        return true;
    }

    void GpuBufferReadbackRing::Release()
    {
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            if (m_mapped[i]) {
                const D3D12_RANGE noWrite{ 0, 0 };
                m_slots[i]->Unmap(0, &noWrite);
                m_mapped[i] = nullptr;
            }
            m_slots[i].Reset();
        }
        m_bytes = 0;
        m_records = 0;
    }

    bool GpuBufferReadbackRing::Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* source,
                                       D3D12_RESOURCE_STATES sourceState)
    {
        if (!cl || !source || m_bytes == 0) {
            return false;
        }
        const uint32_t slot = static_cast<uint32_t>(m_records % SLOT_COUNT);
        auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->ResourceBarrier(1, &toCopy);
        cl->CopyBufferRegion(m_slots[slot].Get(), 0, source, 0, m_bytes);
        auto toBack = CD3DX12_RESOURCE_BARRIER::Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
        cl->ResourceBarrier(1, &toBack);
        ++m_records;
        return true;
    }

    const void* GpuBufferReadbackRing::Latest(uint64_t& outRecord) const
    {
        if (m_bytes == 0 || m_records <= READ_LAG) {
            return nullptr;
        }
        outRecord = m_records - 1 - READ_LAG;
        return m_mapped[outRecord % SLOT_COUNT];
    }
}
