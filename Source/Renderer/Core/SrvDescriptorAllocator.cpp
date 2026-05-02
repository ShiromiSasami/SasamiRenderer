#define NOMINMAX
#include "Renderer/Core/SrvDescriptorAllocator.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    bool SrvDescriptorAllocator::Initialize(IRHIDevice& device, UINT capacity)
    {
        m_device = &device;
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = capacity;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = m_device->CreateDescriptorHeap(srvHeapDesc, m_heap);
        if (FAILED(hr)) {
            return false;
        }
        m_capacity = capacity;
        m_next = 0;
        return true;
    }

    bool SrvDescriptorAllocator::Allocate(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)
    {
        if (!m_device || !m_heap.Get() || count == 0) {
            return false;
        }
        if (m_next + count > m_capacity) {
            DebugLog("SRV heap exhausted\n");
            return false;
        }

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        const auto gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();

        outCpu.ptr = cpuStart.ptr + static_cast<SIZE_T>(m_next) * inc;
        outGpu.ptr = gpuStart.ptr + static_cast<SIZE_T>(m_next) * inc;
        m_next += count;
        return true;
    }

    UINT SrvDescriptorAllocator::GetIndex(GpuDescriptorHandle handle) const
    {
        if (!m_device || !m_heap.Get()) {
            return 0u;
        }

        const GpuDescriptorHandle base = m_heap->GetGPUDescriptorHandleForHeapStart();
        const UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (descriptorSize == 0u || handle.ptr < base.ptr) {
            return 0u;
        }
        return static_cast<UINT>((handle.ptr - base.ptr) / descriptorSize);
    }
}
