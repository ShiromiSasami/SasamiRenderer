// RendererScreenshot.cpp
// Debug screenshot plumbing: queues a back-buffer copy on the next submitted frame and
// encodes it to PNG once the GPU has actually executed that copy.
#define NOMINMAX
#include "Renderer/Runtime/Renderer.h"

#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    namespace
    {
        std::string ToUtf8(const std::wstring& wide)
        {
            if (wide.empty()) {
                return {};
            }

            const int required = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                                        nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                return {};
            }

            std::string utf8(static_cast<size_t>(required), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                  utf8.data(), required, nullptr, nullptr);
            return utf8;
        }
    }

    void Renderer::RequestScreenshot(const std::wstring& path)
    {
        m_screenshotPath = path;
        m_screenshotRequested = true;

        // A stale result from a previous request must not be picked up by a fresh poll.
        m_screenshotResultReady = false;
        m_screenshotResult.clear();
    }

    bool Renderer::ConsumeScreenshotResult(std::string& outMessage)
    {
        if (!m_screenshotResultReady) {
            return false;
        }

        outMessage = m_screenshotResult;
        m_screenshotResult.clear();
        m_screenshotResultReady = false;
        return true;
    }

    void Renderer::RecordPendingScreenshotCopy(CommandList* cmdList, UINT backIndex)
    {
        // A failed record must never leave a stale "copy recorded" flag behind.
        m_screenshotCopyRecorded = false;

        if (!m_screenshotRequested) {
            return;
        }

        const auto fail = [this](const char* message) {
            m_screenshotResult = message;
            m_screenshotResultReady = true;
            m_screenshotRequested = false;
        };

        if (!cmdList || !m_device || !m_device->GetDevice()) {
            fail("ERR renderer device unavailable");
            return;
        }

        // Screenshots are a rare debug operation, so re-initializing on every request (rather
        // than tracking init state with an extra member) is cheap enough to not matter.
        // Initialize() begins with Shutdown() internally and is safe to call repeatedly.
        if (!m_backBufferCapture.Initialize(m_device->GetDevice())) {
            fail("ERR capture init failed");
            return;
        }

        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        if (!backBuffer || !backBuffer->IsValid()) {
            fail("ERR back buffer unavailable");
            return;
        }

        // Every SubmitAndPresent call site (main frame graph, both graph-failure fallbacks,
        // and RenderBootFrame) reaches this with the back buffer already in PRESENT state.
        D3D12_RESOURCE_BARRIER toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer->Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->ResourceBarrier(1, &toCopySource);

        const bool recorded = m_backBufferCapture.RecordCopy(cmdList->Get(), backBuffer->Get());

        // The COPY_SOURCE -> PRESENT restore must run even on failure: the transition to
        // COPY_SOURCE above already executed on the command list.
        D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer->Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        cmdList->ResourceBarrier(1, &toPresent);

        if (!recorded) {
            fail("ERR record copy failed");
            return;
        }

        // The PNG has not been written yet; do not report a result until ResolvePendingScreenshot.
        m_screenshotCopyRecorded = true;
        m_screenshotRequested = false;
    }

    void Renderer::ResolvePendingScreenshot()
    {
        if (!m_screenshotCopyRecorded) {
            return;
        }
        m_screenshotCopyRecorded = false;

        // SaveToPng maps the readback buffer; the copy must have actually finished executing
        // on the GPU first, or mapping returns garbage or a torn frame.
        WaitForGPU();

        const bool ok = m_backBufferCapture.SaveToPng(m_screenshotPath);
        if (ok) {
            const std::string utf8Path = ToUtf8(m_screenshotPath);
            m_screenshotResult = "OK " + utf8Path;
            DebugLog(("[Screenshot] OK " + utf8Path + "\n").c_str());
        } else {
            m_screenshotResult = "ERR png encode failed (see log)";
            DebugLog("[Screenshot] failed to encode PNG\n");
        }
        m_screenshotResultReady = true;
    }
}
