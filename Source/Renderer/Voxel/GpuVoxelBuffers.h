#pragma once

#include "Renderer/RHI/RhiDevice.h"

#include <cstdint>
#include <vector>

// Small buffer helpers shared by the one-shot GPU voxelizers (GpuSurfaceVoxelizer,
// GpuSolidVoxelizer).
namespace SasamiEngine::Render::Voxel::GpuVoxelBuffers
{
    // Destroys every buffer it was given when it leaves scope, whatever the exit path.
    class BufferGuard
    {
    public:
        explicit BufferGuard(IRhiDevice& device) : m_device(device) {}
        ~BufferGuard()
        {
            for (const RhiBufferHandle& buffer : m_buffers)
            {
                if (buffer.IsValid())
                {
                    m_device.DestroyRhiResource(buffer);
                }
            }
        }
        BufferGuard(const BufferGuard&) = delete;
        BufferGuard& operator=(const BufferGuard&) = delete;

        RhiBufferHandle Create(const RhiBufferDesc& desc, const void* initialData)
        {
            const RhiBufferHandle buffer = m_device.CreateRhiBuffer(desc, initialData);
            if (buffer.IsValid())
            {
                m_buffers.push_back(buffer);
            }
            return buffer;
        }

    private:
        IRhiDevice& m_device;
        std::vector<RhiBufferHandle> m_buffers;
    };

    inline RhiBufferDesc MakeBufferDesc(uint64_t sizeInBytes,
                                        uint32_t strideInBytes,
                                        RhiBufferUsageFlags usage,
                                        RhiMemoryUsage memoryUsage,
                                        RhiResourceState initialState)
    {
        RhiBufferDesc desc{};
        desc.sizeInBytes = sizeInBytes;
        desc.strideInBytes = strideInBytes;
        desc.usage = usage;
        desc.memoryUsage = memoryUsage;
        desc.initialState = initialState;
        return desc;
    }

    inline RhiBufferViewDesc MakeStructuredView(uint64_t sizeInBytes, uint32_t strideInBytes)
    {
        RhiBufferViewDesc view{};
        view.type = RhiBufferViewType::Structured;
        view.offset = 0;
        view.sizeInBytes = sizeInBytes;
        view.strideInBytes = strideInBytes;
        return view;
    }
}
