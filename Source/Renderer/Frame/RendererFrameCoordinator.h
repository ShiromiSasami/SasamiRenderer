#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
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
            Resource boneCB;
            uint8_t* boneCBPtr = nullptr;
            UINT boneCbCapacity = 0; // number of skinned object slots
            UINT boneCbCount = 0;
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

        // Bone matrix CB ring buffer  Eeach slot holds Skeleton::kMaxBones float4x4 matrices (8192 bytes)
        void EnsureBoneBuffers(FrameContext& frame, UINT requiredCount);
        D3D12_GPU_VIRTUAL_ADDRESS PushBoneCB(FrameContext& frame, const float* boneMatrices);
        D3D12_GPU_VIRTUAL_ADDRESS PushCameraCB(FrameContext& frame,
                                               const float mvp[16],
                                               const float world[16],
                                               const float extra0[4],
                                               const float extra1[4],
                                               const float extra2[4],
                                               const float extra3[4] = nullptr,
                                               const float extra4[4] = nullptr,
                                               const float extra5[4] = nullptr,
                                               const float extra6[4] = nullptr);

    private:
        bool WaitForFrameFence(UINT frameIndex);

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
