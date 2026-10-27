// RendererComputeQueue.cpp
// The renderer's cross-queue compute fence protocol: WaitForComputeFrameFence waits until a
// frame slot's compute allocator/command list is safe to reset, and SubmitFrameComputeCommands
// closes, executes and signals this frame's compute work. The wait and the signal operate on the
// same members (m_crossQueueFence, m_crossQueueFenceVal, m_computeFrameFenceValues,
// m_computeFrameFenceEvent) and are one responsibility, so they live together here rather than
// being split across the frame code that calls each half — the wait is called from
// RendererPassServices.cpp and the signal from Renderer::Render.
#include "Renderer/Runtime/Renderer.h"

#include <cstdint>

// windows.h (pulled in transitively by the D3D12 headers) defines min/max as macros. Same
// guard as RendererFrameGraph.cpp.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace SasamiEngine::Render
{
    bool Renderer::WaitForComputeFrameFence(UINT frameIndex)
    {
        if (!m_crossQueueFence || !m_computeFrameFenceEvent || frameIndex >= m_computeFrameFenceValues.size()) {
            return false;
        }

        const UINT64 fenceValue = m_computeFrameFenceValues[frameIndex];
        if (fenceValue == 0) {
            return true;
        }

        if (m_crossQueueFence->GetCompletedValue() < fenceValue) {
            if (FAILED(m_crossQueueFence->SetEventOnCompletion(fenceValue, m_computeFrameFenceEvent))) {
                return false;
            }
            const DWORD waitResult = WaitForSingleObject(m_computeFrameFenceEvent, INFINITE);
            if (waitResult != WAIT_OBJECT_0) {
                return false;
            }
        }
        return true;
    }

    void Renderer::SubmitFrameComputeCommands(CommandList* computeCmdList, UINT computeSlot)
    {
        // Submit compute CL (SWRT work) on the compute queue. Any mid-frame
        // cross-queue join for this frame (RenderGraph::Execute's Signal/Wait pair)
        // has already ordered this submission relative to the graphics queue's Node
        // submissions above; it does not by itself order this *frame-closing*
        // submission against anything after it. Signal here and record the value
        // so a later frame's WaitForComputeFrameFence() can safely Reset() this
        // slot's allocator/command list once the GPU has actually finished this.
        if (computeCmdList) {
            if (SUCCEEDED(computeCmdList->Close())) {
                ID3D12CommandList* cLists[] = { computeCmdList->Get() };
                m_device->GetComputeQueue().ExecuteCommandLists(1, cLists);
                if (m_crossQueueFence && computeSlot < m_computeFrameFenceValues.size()) {
                    const UINT64 fenceValue = ++m_crossQueueFenceVal;
                    if (SUCCEEDED(m_device->GetComputeQueue().Signal(m_crossQueueFence.Get(), fenceValue))) {
                        m_computeFrameFenceValues[computeSlot] = fenceValue;
                    }
                }
            }
        }
    }
}
