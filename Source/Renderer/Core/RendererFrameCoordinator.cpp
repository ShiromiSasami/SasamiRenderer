#include "Renderer/Core/RendererFrameCoordinator.h"

#include <cstring>
#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"

namespace
{
    struct CameraCBData
    {
        float mvp[16];
        float world[16];
        float extra0[4];
        float extra1[4];
        float extra2[4];
        float extra3[4];
    };
}

namespace SasamiRenderer
{
    bool RendererFrameCoordinator::Initialize(IRHIDevice& device,
                                              RenderPipelineStateCache& pipelineStateCache,
                                              LightSystem& lightSystem,
                                              UINT frameCount,
                                              const AllocateSrvRangeCallback& allocateSrvRange)
    {
        if (frameCount == 0) {
            DebugLogDialog("RendererFrameCoordinator::Initialize: frameCount must be greater than zero.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        m_device = &device;
        m_pipelineStateCache = &pipelineStateCache;

        m_frames.clear();
        m_frames.resize(frameCount);

        const UINT cameraCbSize = (sizeof(CameraCBData) + 255u) & ~255u;
        for (UINT i = 0; i < frameCount; ++i) {
            auto& frame = m_frames[i];

            HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, frame.cmdAllocator);
            if (FAILED(hr)) {
                DebugLogDialog("RendererFrameCoordinator::Initialize: CreateCommandAllocator failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }

            if (!ResourceUploadUtility::CreateUploadBuffer(*m_device,
                                                           cameraCbSize,
                                                           frame.cameraCB,
                                                           reinterpret_cast<void**>(&frame.cameraCBPtr))) {
                DebugLogDialog("RendererFrameCoordinator::Initialize: Camera CB creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }
            frame.cameraCbCapacity = 1;
            frame.cameraCbCount = 0;

            if (!lightSystem.InitializeFrameResources(frame.light, allocateSrvRange)) {
                return false;
            }

            frame.fenceValue = 0;
        }

        HRESULT hr = m_device->CreateCommandList(0,
                                                 D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 m_frames[0].cmdAllocator,
                                                 &m_pipelineStateCache->GetPipelineState(),
                                                 m_mainCommandList);
        if (FAILED(hr)) {
            DebugLogDialog("RendererFrameCoordinator::Initialize: Main command list creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_mainCommandList.Close();
        m_mainCommandListReady = true;

        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, m_frameFence.GetAddressOf());
        if (FAILED(hr)) {
            DebugLogDialog("RendererFrameCoordinator::Initialize: Frame fence creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        m_frameFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_frameFenceEvent) {
            DebugLogDialog("RendererFrameCoordinator::Initialize: Frame fence event creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        return true;
    }

    void RendererFrameCoordinator::Shutdown(LightSystem& lightSystem)
    {
        for (auto& frame : m_frames) {
            if (frame.cameraCB.IsValid() && frame.cameraCBPtr) {
                frame.cameraCB->Unmap(0, nullptr);
                frame.cameraCBPtr = nullptr;
                frame.cameraCbCapacity = 0;
                frame.cameraCbCount = 0;
            }
            lightSystem.ShutdownFrameResources(frame.light);
        }
        m_frames.clear();

        if (m_frameFenceEvent) {
            CloseHandle(m_frameFenceEvent);
            m_frameFenceEvent = nullptr;
        }

        m_frameFence.Reset();
        m_mainCommandList = {};
        m_mainCommandListReady = false;
        m_nextFenceValue = 1;
        m_pipelineStateCache = nullptr;
        m_device = nullptr;
    }

    RendererFrameCoordinator::FrameContext* RendererFrameCoordinator::GetFrameContext(UINT frameIndex)
    {
        if (frameIndex >= m_frames.size()) {
            return nullptr;
        }
        return &m_frames[frameIndex];
    }

    const RendererFrameCoordinator::FrameContext* RendererFrameCoordinator::GetFrameContext(UINT frameIndex) const
    {
        if (frameIndex >= m_frames.size()) {
            return nullptr;
        }
        return &m_frames[frameIndex];
    }

    void RendererFrameCoordinator::WaitForFrameFence(UINT frameIndex)
    {
        if (!m_frameFence || frameIndex >= m_frames.size()) {
            return;
        }

        const UINT64 fenceValue = m_frames[frameIndex].fenceValue;
        if (fenceValue == 0) {
            return;
        }

        if (m_frameFence->GetCompletedValue() < fenceValue) {
            if (FAILED(m_frameFence->SetEventOnCompletion(fenceValue, m_frameFenceEvent))) return;
            WaitForSingleObject(m_frameFenceEvent, 5000);
        }
    }

    bool RendererFrameCoordinator::BeginFrame(UINT frameIndex, CommandList*& outCmdList)
    {
        outCmdList = nullptr;
        if (frameIndex >= m_frames.size() || !m_mainCommandListReady || !m_pipelineStateCache) {
            return false;
        }

        WaitForFrameFence(frameIndex);

        HRESULT hr = m_frames[frameIndex].cmdAllocator.Reset();
        if (FAILED(hr)) {
            DebugLog("RendererFrameCoordinator::BeginFrame: Failed to reset command allocator.\n");
            return false;
        }

        hr = m_mainCommandList.Reset(m_frames[frameIndex].cmdAllocator, &m_pipelineStateCache->GetPipelineState());
        if (FAILED(hr)) {
            DebugLog("RendererFrameCoordinator::BeginFrame: Failed to reset command list.\n");
            return false;
        }

        outCmdList = &m_mainCommandList;
        return true;
    }

    void RendererFrameCoordinator::SignalFrame(UINT frameIndex)
    {
        if (!m_frameFence || !m_device || frameIndex >= m_frames.size()) {
            return;
        }

        const UINT64 fenceValue = m_nextFenceValue++;
        HRESULT hr = m_device->GetCommandQueue().Signal(m_frameFence.Get(), fenceValue);
        if (SUCCEEDED(hr)) {
            m_frames[frameIndex].fenceValue = fenceValue;
        }
    }

    UINT64 RendererFrameCoordinator::SignalQueueFence()
    {
        if (!m_frameFence || !m_device) {
            return 0;
        }

        const UINT64 fenceValue = m_nextFenceValue++;
        const HRESULT hr = m_device->GetCommandQueue().Signal(m_frameFence.Get(), fenceValue);
        if (FAILED(hr)) {
            return 0;
        }

        return fenceValue;
    }

    bool RendererFrameCoordinator::IsFenceComplete(UINT64 fenceValue) const
    {
        if (!m_frameFence || fenceValue == 0) {
            return true;
        }

        return m_frameFence->GetCompletedValue() >= fenceValue;
    }

    void RendererFrameCoordinator::ResetFrameFenceValues()
    {
        for (auto& frame : m_frames) {
            frame.fenceValue = 0;
        }
    }

    void RendererFrameCoordinator::EnsureCameraBuffers(FrameContext& frame, UINT requiredCount)
    {
        if (!m_device) {
            return;
        }

        const UINT cbSize = (sizeof(CameraCBData) + 255u) & ~255u;
        const UINT needed = (requiredCount > 0) ? requiredCount : 1u;
        if (frame.cameraCB.IsValid() &&
            frame.cameraCBPtr &&
            frame.cameraCbCapacity >= needed) {
            frame.cameraCbCount = 0;
            return;
        }

        UINT newCapacity = (frame.cameraCbCapacity > 0) ? frame.cameraCbCapacity : 1u;
        while (newCapacity < needed) {
            newCapacity *= 2u;
        }

        if (frame.cameraCB.IsValid() && frame.cameraCBPtr) {
            frame.cameraCB->Unmap(0, nullptr);
            frame.cameraCBPtr = nullptr;
        }
        frame.cameraCB.Reset();
        frame.cameraCbCapacity = 0;
        frame.cameraCbCount = 0;

        if (!ResourceUploadUtility::CreateUploadBuffer(*m_device,
                                                       static_cast<UINT64>(cbSize) * static_cast<UINT64>(newCapacity),
                                                       frame.cameraCB,
                                                       reinterpret_cast<void**>(&frame.cameraCBPtr))) {
            DebugLog("RendererFrameCoordinator::EnsureCameraBuffers: Camera CB grow failed.\n");
            return;
        }
        frame.cameraCbCapacity = newCapacity;
    }

    D3D12_GPU_VIRTUAL_ADDRESS RendererFrameCoordinator::PushCameraCB(FrameContext& frame,
                                                                     const float mvp[16],
                                                                     const float world[16])
    {
        return PushCameraCB(frame, mvp, world, nullptr, nullptr, nullptr);
    }

    D3D12_GPU_VIRTUAL_ADDRESS RendererFrameCoordinator::PushCameraCB(FrameContext& frame,
                                                                     const float mvp[16],
                                                                     const float world[16],
                                                                     const float extra0[4],
                                                                     const float extra1[4],
                                                                     const float extra2[4],
                                                                     const float extra3[4])
    {
        if (!frame.cameraCB.IsValid() || !frame.cameraCBPtr || frame.cameraCbCapacity == 0) {
            return 0;
        }

        const UINT cbSize = (sizeof(CameraCBData) + 255u) & ~255u;
        UINT slot = frame.cameraCbCount;
        if (slot >= frame.cameraCbCapacity) {
            slot = frame.cameraCbCapacity - 1;
        } else {
            ++frame.cameraCbCount;
        }

        CameraCBData* dst = reinterpret_cast<CameraCBData*>(
            frame.cameraCBPtr + static_cast<size_t>(cbSize) * static_cast<size_t>(slot));
        std::memcpy(dst->mvp, mvp, sizeof(dst->mvp));
        std::memcpy(dst->world, world, sizeof(dst->world));
        if (extra0) {
            std::memcpy(dst->extra0, extra0, sizeof(dst->extra0));
        } else {
            std::memset(dst->extra0, 0, sizeof(dst->extra0));
        }
        if (extra1) {
            std::memcpy(dst->extra1, extra1, sizeof(dst->extra1));
        } else {
            std::memset(dst->extra1, 0, sizeof(dst->extra1));
        }
        if (extra2) {
            std::memcpy(dst->extra2, extra2, sizeof(dst->extra2));
        } else {
            std::memset(dst->extra2, 0, sizeof(dst->extra2));
        }
        if (extra3) {
            std::memcpy(dst->extra3, extra3, sizeof(dst->extra3));
        } else {
            std::memset(dst->extra3, 0, sizeof(dst->extra3));
        }

        return frame.cameraCB->GetGPUVirtualAddress() + static_cast<UINT64>(cbSize) * static_cast<UINT64>(slot);
    }
}
