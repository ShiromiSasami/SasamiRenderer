#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include <cstdint>

namespace SasamiRenderer
{
    class SrvDescriptorAllocator
    {
    public:
        bool Initialize(IRHIDevice& device, UINT capacity);

        bool Allocate(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu);

        UINT GetIndex(GpuDescriptorHandle handle) const;

        DescriptorHeap*       GetHeap()       { return &m_heap; }
        const DescriptorHeap* GetHeap() const { return &m_heap; }

    private:
        IRHIDevice*    m_device   = nullptr;
        DescriptorHeap m_heap;
        UINT           m_capacity = 0u;
        UINT           m_next     = 0u;
    };
}
