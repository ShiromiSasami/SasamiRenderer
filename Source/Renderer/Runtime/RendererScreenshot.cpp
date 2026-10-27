// RendererScreenshot.cpp
// Debug screenshot plumbing: queues a back-buffer copy on the next submitted frame and
// encodes it to PNG once the GPU has actually executed that copy.
#define NOMINMAX
#include "Renderer/Runtime/Renderer.h"

#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

namespace SasamiEngine::Render
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

        bool IsKnownCaptureTargetName(const std::string& name)
        {
            static const char* const kKnownNames[] = {
                "backbuffer",
                "shadow0", "shadow1", "shadow2", "shadow3",
                "gbuffer.albedo", "gbuffer.normal", "gbuffer.material", "gbuffer.emissive", "gbuffer.specular",
            };
            for (const char* candidate : kKnownNames) {
                if (name == candidate) {
                    return true;
                }
            }
            return false;
        }

        struct ResolvedCaptureTarget
        {
            ID3D12Resource* resource = nullptr;
            UINT subresource = 0;
            D3D12_RESOURCE_STATES restingState = D3D12_RESOURCE_STATE_COMMON;
        };

        // Resolves a capture target name to a live resource. Must be called at record time --
        // holding a resolved resource pointer across frames would go stale across a resize.
        ResolvedCaptureTarget ResolveCaptureTarget(const std::string& name, RenderTargetPool& renderTargetPool,
                                                    LightSystem& lightSystem, UINT backIndex)
        {
            ResolvedCaptureTarget target;
            if (name == "backbuffer") {
                const auto* backBuffer = renderTargetPool.GetBackBufferResource(backIndex);
                if (backBuffer && backBuffer->IsValid()) {
                    target.resource = backBuffer->Get();
                }
                target.subresource = 0;
                target.restingState = D3D12_RESOURCE_STATE_PRESENT;
            } else if (name.size() == 7 && name.compare(0, 6, "shadow") == 0 && name[6] >= '0' && name[6] <= '3') {
                Resource& shadowMap = lightSystem.GetShadowMapManager().GetShadowMap();
                if (shadowMap.IsValid()) {
                    target.resource = shadowMap.Get();
                }
                target.subresource = static_cast<UINT>(name[6] - '0');
                target.restingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            } else if (name == "gbuffer.albedo") {
                Resource& res = renderTargetPool.GetGBufferAlbedo();
                if (res.IsValid()) {
                    target.resource = res.Get();
                }
                target.subresource = 0;
                target.restingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            } else if (name == "gbuffer.normal") {
                Resource& res = renderTargetPool.GetGBufferNormal();
                if (res.IsValid()) {
                    target.resource = res.Get();
                }
                target.subresource = 0;
                target.restingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            } else if (name == "gbuffer.material") {
                Resource& res = renderTargetPool.GetGBufferMaterial();
                if (res.IsValid()) {
                    target.resource = res.Get();
                }
                target.subresource = 0;
                target.restingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            } else if (name == "gbuffer.emissive") {
                Resource& res = renderTargetPool.GetGBufferEmissive();
                if (res.IsValid()) {
                    target.resource = res.Get();
                }
                target.subresource = 0;
                target.restingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            } else if (name == "gbuffer.specular") {
                Resource& res = renderTargetPool.GetGBufferSpecularWorkflow();
                if (res.IsValid()) {
                    target.resource = res.Get();
                }
                target.subresource = 0;
                target.restingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            return target;
        }
    }

    std::string Renderer::RequestTargetDump(const std::string& name, const std::wstring& path)
    {
        // Request acceptance is backend-agnostic; RendererFrameGraph.cpp's native-backend
        // frame path completes (or rejects) requests it cannot fulfil itself.
        if (!IsKnownCaptureTargetName(name)) {
            return "unknown capture target: " + name;
        }

        // Only one capture can be outstanding at a time -- the poll protocol (screenshot
        // status) assumes a single request/result pair.
        if (m_screenshotRequested) {
            return "capture already in progress";
        }

        m_screenshotPath = path;
        m_captureTargetName = name;
        m_screenshotRequested = true;

        // A stale result from a previous request must not be picked up by a fresh poll.
        m_screenshotResultReady = false;
        m_screenshotResult.clear();

        return {};
    }

    void Renderer::RequestScreenshot(const std::wstring& path)
    {
        RequestTargetDump("backbuffer", path);
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
        if (!m_textureCapture.Initialize(m_device->GetDevice())) {
            fail("ERR capture init failed");
            return;
        }

        const ResolvedCaptureTarget target =
            ResolveCaptureTarget(m_captureTargetName, m_renderTargetPool, m_lightSystem, backIndex);
        if (!target.resource) {
            fail("ERR capture target unavailable");
            return;
        }

        // Every SubmitAndPresent call site (main frame graph, both graph-failure fallbacks,
        // and RenderBootFrame) reaches this with the back buffer already in PRESENT state.
        D3D12_RESOURCE_BARRIER toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
            target.resource, target.restingState, D3D12_RESOURCE_STATE_COPY_SOURCE, target.subresource);
        cmdList->ResourceBarrier(1, &toCopySource);

        const bool recorded = m_textureCapture.RecordCopy(cmdList->Get(), target.resource, target.subresource);

        // The COPY_SOURCE -> resting-state restore must run even on failure: the transition to
        // COPY_SOURCE above already executed on the command list.
        D3D12_RESOURCE_BARRIER toResting = CD3DX12_RESOURCE_BARRIER::Transition(
            target.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, target.restingState, target.subresource);
        cmdList->ResourceBarrier(1, &toResting);

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

        const bool ok = m_textureCapture.SaveToPng(m_screenshotPath);
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
