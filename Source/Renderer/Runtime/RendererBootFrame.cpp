// RendererBootFrame.cpp
// Renderer::RenderBootFrame -- minimal presentable frame used while progressive boot
// deferred tasks are still draining, so the window keeps painting instead of showing a
// frozen/white surface. Mirrors the begin/end skeleton of Renderer::Render's D3D12
// compatibility-surface path (see RendererFrameGraph.cpp / RendererGraphicsCommands.cpp)
// with a minimal body: clear + optional overlay (loading UI). Backends that own their own
// frame take the ExecuteBackendFrame path instead, with the same clear and overlay.
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

namespace SasamiEngine::Render
{
    void Renderer::RenderBootFrame(const OverlayRenderCallback& overlay)
    {
        ScopedPerfTimer perfTimer("Renderer::RenderBootFrame");

        if (!m_device) {
            return;
        }

        if (!m_device->GetCapabilities().supportsD3D12CompatibilitySurface) {
            // Backends that own their whole frame get the same treatment as a normal frame, minus
            // the scene: a clear plus the overlay. The caller has already built the ImGui frame for
            // the boot window, so the overlay draws exactly what the D3D12 path draws -- previously
            // this returned immediately and progress only appeared in the title bar.
            //
            // Leaving mesh and rayMarch at their defaults is what selects the clear-only path
            // inside each backend; there is no scene to draw yet by definition.
            RhiBackendFrameDesc frameDesc{};
            frameDesc.clearColor = { 0.06f, 0.06f, 0.08f, 1.0f };
            frameDesc.overlay = m_frameOverlay;
            frameDesc.present = true;
            if (!m_device->ExecuteBackendFrame(frameDesc)) {
                DebugLog("Renderer::RenderBootFrame: native backend frame execution failed.\n");
            }
            return;
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

} // namespace SasamiEngine::Render
