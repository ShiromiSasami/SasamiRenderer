#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Scene/LightSystem.h"

#include <functional>
#include <vector>

namespace SasamiRenderer
{
    class RendererFrameCoordinator
    {
    public:
        struct FrameContext
        {
            CommandAllocator cmdAllocator;
            Resource cameraCB;
            uint8_t* cameraCBPtr = nullptr;
            UINT cameraCbCapacity = 0;
            UINT cameraCbCount = 0;
            LightSystem::FrameResources light;
            UINT64 fenceValue = 0;
        };

        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                            CpuDescriptorHandle& outCpu,
                                                            GpuDescriptorHandle& outGpu)>;

        bool Initialize(IRHIDevice& device,
                        RenderPipelineStateCache& pipelineStateCache,
                        LightSystem& lightSystem,
                        UINT frameCount,
                        const AllocateSrvRangeCallback& allocateSrvRange);

        void Shutdown(LightSystem& lightSystem);

        bool BeginFrame(UINT frameIndex, CommandList*& outCmdList);
        void SignalFrame(UINT frameIndex);
        UINT64 SignalQueueFence();
        bool IsFenceComplete(UINT64 fenceValue) const;
        void ResetFrameFenceValues();

        FrameContext* GetFrameContext(UINT frameIndex);
        const FrameContext* GetFrameContext(UINT frameIndex) const;

        void EnsureCameraBuffers(FrameContext& frame, UINT requiredCount);
        D3D12_GPU_VIRTUAL_ADDRESS PushCameraCB(FrameContext& frame, const float mvp[16], const float world[16]);

    private:
        void WaitForFrameFence(UINT frameIndex);

        IRHIDevice* m_device = nullptr;
        RenderPipelineStateCache* m_pipelineStateCache = nullptr;

        std::vector<FrameContext> m_frames;
        CommandList m_mainCommandList;
        bool m_mainCommandListReady = false;

        ComPtr<ID3D12Fence> m_frameFence;
        HANDLE m_frameFenceEvent = nullptr;
        UINT64 m_nextFenceValue = 1;
    };
}
