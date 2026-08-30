// RendererBootFrame.cpp
// Renderer::RenderBootFrame -- minimal presentable frame used while progressive boot
// deferred tasks are still draining, so the window keeps painting instead of showing a
// frozen/white surface. Mirrors the begin/end skeleton of Renderer::Render's D3D12
// compatibility-surface path (see RendererFrameGraph.cpp / RendererGraphicsCommands.cpp)
// with a minimal body: clear + optional overlay (loading UI).
#define NOMINMAX
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/SceneSynchronizer.h"
#include "Renderer/Scene/EnvironmentManager.h"

#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    void Renderer::RenderBootFrame(const OverlayRenderCallback& overlay)
    {
        ScopedPerfTimer perfTimer("Renderer::RenderBootFrame");

        if (!m_device || !m_device->GetCapabilities().supportsD3D12CompatibilitySurface) {
            return; // Non-D3D12 backends: no-op, the app falls back to title-bar progress.
        }

        const UINT backIndex = m_device->GetSwapChain()->GetCurrentBackBufferIndex();
        auto* frame = m_frameCoordinator.GetFrameContext(backIndex);
        if (!frame) {
            return; // Frame infrastructure not initialized yet.
        }

        CommandList* cmdList = nullptr;
        if (!m_frameCoordinator.BeginFrame(backIndex, cmdList) || !cmdList) {
            return;
        }

        TransitionBackBufferToRenderTarget(cmdList, backIndex);

        auto rtvHandle = m_renderTargetPool.GetBackBufferRtv(backIndex);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        cmdList->RSSetViewports(1, &m_viewport);
        cmdList->RSSetScissorRects(1, &m_scissorRect);

        const FLOAT clearColor[] = { 0.06f, 0.06f, 0.08f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        if (overlay) {
            overlay(*cmdList, rtvHandle);
        }

        TransitionBackBufferToPresent(cmdList, backIndex);

        SubmitAndPresent(cmdList, backIndex);
    }

} // namespace SasamiRenderer
