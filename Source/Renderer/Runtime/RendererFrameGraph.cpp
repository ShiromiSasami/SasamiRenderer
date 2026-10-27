// RendererFrameGraph.cpp
// Renderer::Render -- the per-frame sequence itself, and nothing else. Its single
// responsibility is deciding what happens in what order: open the frame, run each stage,
// register the passes, execute the graph, and close the frame by presenting.
//
// Everything a stage *does* lives elsewhere, one responsibility per file, because each of
// those is a decision made from its own inputs rather than from the frame's ordering:
//   - RendererNativeFrame.cpp        the whole non-DX12 frame (never touches this graph)
//   - RendererCameraBudget.cpp       sizing the frame's per-draw camera constant buffer
//   - RendererFrameTargets.cpp       ensuring and importing this frame's render targets
//   - RendererPostProcessPasses.cpp  configuring the screen-space post-process passes
//   - RendererDrawCallbacks.cpp      stage 4: the draw/shadow-draw callbacks
//   - RendererPassServices.cpp       stage 5: pass inputs, services and the execute context
//   - RendererComputeQueue.cpp       the cross-queue compute fence, wait and signal
//   - RendererGIBakeStep.cpp         advancing a requested GI probe bake by one frame
//   - RendererGraphicsCommands.cpp   the individual recorded command sequences
//
// What stays here is only what the ordering itself owns: the back-buffer index and the
// frame context, the GPU-profiler frame ring (whose correctness is a statement about order
// relative to BeginFrame), the graph's Clear/Execute/Clear lifecycle, the two
// pass-registration failure paths that still have to present something, and the frame-tail
// simulation steps that are deliberately after the graph read their state.
#include "Renderer/Runtime/Renderer.h"

#include <cstdio>
#include "Renderer/Scene/SceneSynchronizer.h"
#include "Renderer/Scene/EnvironmentManager.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <windowsx.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Foundation/Profiling/Profiler.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"

using namespace std;


namespace SasamiEngine::Render
{
    void Renderer::Render(const OverlayRenderCallback& overlay)
    {
        Profiler::ScopedCpuEvent frameEvent("Renderer::Render");

        if (!m_device) {
            return;
        }
        ++m_renderCallCounter;

        // The D3D12 and native paths consume the camera differently -- one bakes an MVP per
        // object, the other hands the backend a view-projection -- so when the two backends
        // disagree about where the camera is standing, this is the first thing to compare.
        // Logged only when it changes, which for a parked camera means once.
        {
            const float* pv = m_cameraState.GetPV();
            char signature[128];
            snprintf(signature,
                     sizeof(signature),
                     "%.3f %.3f %.3f %.3f %.3f %.3f",
                     pv[0], pv[5], pv[10], pv[12], pv[13], pv[14]);
            if (m_lastLoggedCameraSignature != signature) {
                m_lastLoggedCameraSignature = signature;
                char message[224];
                snprintf(message,
                         sizeof(message),
                         "Renderer::Render: viewProjection diag/translation row = %s\n",
                         signature);
                DebugLog(message);
            }
        }

        // The whole non-DX12 frame is handled in RendererNativeFrame.cpp (see Renderer.h for why
        // it lives in a separate file). A true return means that path fully executed the frame.
        if (RenderNativeBackendFrame(overlay)) {
            return;
        }

        RetireDeferredUploadBatches();

        if (m_deltaTime > 0.0f) {
            m_sceneTime += m_deltaTime;
        }

        const UINT backIndex = m_device->GetSwapChain()->GetCurrentBackBufferIndex();
        auto* frame = m_frameCoordinator.GetFrameContext(backIndex);
        if (!frame) {
            return;
        }

        CommandList* cmdList = nullptr;
        if (!m_frameCoordinator.BeginFrame(backIndex, cmdList)) {
            return;
        }

        // BeginFrame above waited on the fence for this back-buffer slot, i.e. the GPU has
        // finished frame (counter - GetBackBufferCount()). kFrameLatency is >= that, so the
        // frame we read here finished even earlier and its readback memory is settled.
        //
        // Order matters: kFrameLatency == the ring size, so (counter - kFrameLatency) maps to
        // the very slot BeginFrame is about to reuse. Reading it after would return this
        // frame's uninitialized metadata instead of the completed frame's results.
        // Increment first: the counter must already name the frame about to be recorded, because
        // the slot BeginFrame is about to claim is exactly the one holding frame
        // (thisFrame - kFrameLatency). Reading before the increment asks for one frame further
        // back than the ring actually still holds, and UpdateResults' appFrameIndex guard
        // (correctly) rejects every such request, leaving the results permanently empty.
        ++m_gpuProfilerFrameCounter;
        if (m_gpuProfilerFrameCounter >= GpuTimestampProfiler::kFrameLatency) {
            m_gpuTimestampProfiler.UpdateResults(
                m_gpuProfilerFrameCounter - GpuTimestampProfiler::kFrameLatency);
        }
        m_gpuTimestampProfiler.BeginFrame(m_gpuProfilerFrameCounter);
        EnsureFrameCameraConstantBuffers(*frame);

        m_environmentManager.EnsureTexturesUploaded(cmdList);

        m_renderGraph.Clear();
        if (!EnsureAndImportFrameTargets(backIndex)) {
            return;
        }
        // Configured after the imports rather than between two of them, which is where this used
        // to sit. The order between them is immaterial, but *not* because the two touch disjoint
        // state: both reach into m_renderTargetPool. The real reason is narrower --
        // ConfigurePostProcessPasses' only use of the pool is a read, GetBackBufferResource(), and
        // what it reads is the swap chain's back buffer, which EnsureAndImportFrameTargets neither
        // creates nor resizes. The resources that function's Ensure* calls do allocate (scene
        // colour, transmission copy, OIT, SSR, GBuffer, SSAO) are a separate set that the
        // post-process configuration never looks at. Everything else it writes -- m_passRegistry,
        // the pass objects, m_srvAllocator -- the imports really do not touch, and the registry is
        // not read until RegisterPassesToRenderGraph below.
        //
        // That premise breaks the moment ConfigurePostProcessPasses starts calling an
        // m_renderTargetPool Ensure* of its own, or reads a target the Ensure* calls above
        // allocate: it would then depend on the imports having already run, and this call could no
        // longer be moved freely with respect to them. Re-derive the ordering if that happens
        // instead of trusting this comment.
        ConfigurePostProcessPasses(backIndex);

        // Stages 4-5 (draw-callback construction and render-pass input/service construction)
        // used to be inlined here; they now live in RendererDrawCallbacks.cpp and
        // RendererPassServices.cpp. FrameRenderContext carries the state they build (and
        // that this function and stages 6-9 below still need) safely across that split --
        // see FrameRenderContext.h.
        FrameRenderContext frameContext;
        frameContext.cmdList   = &cmdList;
        frameContext.frame     = frame;
        frameContext.backIndex = backIndex;
        PrepareFrameDrawCallbacks(frameContext);
        BuildFramePassInputsAndServices(frameContext);
        // The GPU-driven cluster path's per-frame preparation (RendererClusterFrame.cpp).
        // Placed here, and not inside a pass, for two reasons that are both about order:
        // it records page-pool copies onto cmdList, so it has to precede every pass that
        // reads the pool (the directional-shadow cascades are the first); and it needs
        // executionPolicy.useMeshShader, which stage 5 above is what computes. Everything
        // before this point either only waits on fences and ensures render targets, or
        // touches resources the page pool and the instance buffer have nothing to do with.
        BeginClusterFrameUpdate(cmdList, backIndex, frameContext.executionPolicy.useMeshShader);
        // Only computeCmdList/computeSlot are still read below (stage 7's compute-queue
        // submit); executionPolicy/frameInputs/executionServices etc. are consumed entirely
        // within stage 5 (BuildFramePassInputsAndServices) and RenderGraph::Execute(), which
        // reads them via frameContext.executeContext, not through a bare local here.
        CommandList* const computeCmdList = frameContext.computeCmdList;
        const UINT computeSlot = frameContext.computeSlot;

        if (!m_passRegistry.RegisterPassesToRenderGraph(m_renderGraph, frameContext.executeContext, m_settings.renderPathMode, m_rayTracingRenderPass)) {
            m_renderGraph.Clear();
            if (overlay) {
                TransitionBackBufferToRenderTarget(cmdList, backIndex);
                ClearAndBindMainTargets(cmdList, backIndex);
                overlay(*cmdList, m_renderTargetPool.GetBackBufferRtv(backIndex));
                TransitionBackBufferToPresent(cmdList, backIndex);
            }
            SubmitAndPresent(cmdList, backIndex);
            return;
        }

        m_passRegistry.RegisterPhaseCompletionNodesToRenderGraph(m_renderGraph, frameContext.executeContext);

        const bool graphExecuted = m_renderGraph.Execute();
        m_renderGraph.Clear();
        // Deliberately before the !graphExecuted return below rather than with the frame-tail
        // steps further down: a graph that fails in a later pass has still run the views that
        // reported streaming feedback (the shadow cascades are near the front), and that
        // feedback is what decides the next frame's resident set. Skipping the close on those
        // frames would stall streaming for as long as any one pass keeps failing.
        EndClusterFrameUpdate();

        SubmitFrameComputeCommands(computeCmdList, computeSlot);

        if (!graphExecuted) {
            if (overlay) {
                TransitionBackBufferToRenderTarget(cmdList, backIndex);
                ClearAndBindMainTargets(cmdList, backIndex);
                overlay(*cmdList, m_renderTargetPool.GetBackBufferRtv(backIndex));
                TransitionBackBufferToPresent(cmdList, backIndex);
            }
            SubmitAndPresent(cmdList, backIndex);
            return;
        }

        CaptureSceneColorHistory(cmdList, backIndex);

        // Advance the fluid heightfield simulation after the render graph has read this
        // frame's state (FluidSurface_VS samples GetCurrentHeightGpuVA/GetUpdateCBGpuVA);
        // the updated buffer becomes visible starting next frame (1-frame lag), mirroring
        // the GI probe update timing below.
        if (m_readyState.IsFeatureReady(m_readyState.fluidReady) && m_fluidSim.IsInitialized() && cmdList) {
            m_fluidSim.Update(m_deltaTime, *m_device, *cmdList);
        }
        // Same one-frame lag as the heightfield: the raymarch pass read this frame's state earlier
        // in the graph, so stepping here makes the result visible next frame.
        if (m_readyState.IsFeatureReady(m_readyState.smokeReady) && m_smokeSim.IsInitialized() && cmdList) {
            m_smokeSim.Update(*cmdList, m_deltaTime);
        }
        if (m_readyState.IsFeatureReady(m_readyState.particlesReady) && m_particleSystem.IsInitialized() && cmdList) m_particleSystem.Update(m_deltaTime, *m_device, *cmdList);
        if (GetCrowdEnabled() && m_crowdSystem.IsInitialized()) {
            std::string crowdError;
            if (!m_crowdSystem.Update(m_deltaTime, crowdError)) {
                static bool s_crowdUpdateErrorLogged = false;
                if (!s_crowdUpdateErrorLogged) {
                    s_crowdUpdateErrorLogged = true;
                    DebugLog(("Renderer: CrowdSystem::Update failed: " + crowdError + "\n").c_str());
                }
            }
        }

        if (m_frameComputeHook && frameContext.graphicsCommandEncoder) {
            m_frameComputeHook(*frameContext.graphicsCommandEncoder);
        }

        StepGIBake(cmdList);

        if (overlay) {
            TransitionBackBufferToRenderTarget(cmdList, backIndex);
            BindMainTargets(cmdList, backIndex);
            overlay(*cmdList, m_renderTargetPool.GetBackBufferRtv(backIndex));
            TransitionBackBufferToPresent(cmdList, backIndex);
        }
        SubmitAndPresent(cmdList, backIndex);
    }


} // namespace SasamiEngine::Render
