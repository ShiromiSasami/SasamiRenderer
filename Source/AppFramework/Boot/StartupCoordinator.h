#pragma once

#include <cstdint>
#include <windows.h>

#include "Foundation/Boot/BootStatus.h"
#include "Foundation/Boot/InitTaskScheduler.h"
#include "Foundation/Boot/MainThreadTaskPump.h"

namespace SasamiRenderer
{
    class ApplicationCore;

    // Boot phase state machine. ApplicationCore::Run() calls Pump() once per main-loop
    // iteration; the coordinator sequences renderer core init, frame-infrastructure init
    // (on a JobSystem worker when the backend allows), the game's OnInit, and the drain
    // of deferred init tasks -- all without ever blocking the Win32 message pump.
    //
    // Phase flow (see BootPhase):
    //   CreatingRendererCore  -> core init steps; window shows title-bar progress
    //   LoadingWithBootScreen -> Renderer::RenderBootFrame + BootProgressWindow;
    //                            deferred tasks drain with a generous time budget
    //   SceneReady            -> normal frame path; remaining tasks drain with a small
    //                            per-frame budget so features light up progressively
    //   Running               -> everything done; boot UI hidden
    class StartupCoordinator
    {
    public:
        // Per-frame time budgets for main-thread init work (MainThreadTaskPump).
        static constexpr double kBootBudgetMs = 100.0; // boot screen at ~10 fps is fine
        static constexpr double kSceneBudgetMs = 4.0;  // keep interactive frame rate

        explicit StartupCoordinator(ApplicationCore& app);

        BootPhase Phase() const { return m_phase; }
        bool HasFailed() const { return m_failed; }

        // Advances the boot flow by one slice. Main thread, once per loop iteration.
        // During SceneReady/Running this only drains remaining tasks (cheap).
        void Pump();

        // Records a resize that arrived before the renderer core exists; applied once
        // the core is ready.
        void NotifyResize(UINT width, UINT height);

        BootStatus GetBootStatusSnapshot() const;

        // Blocks on outstanding worker tasks. Call from ApplicationCore::OnDestroy
        // BEFORE JobSystem::Shutdown().
        void Shutdown();

    private:
        enum class CoreStep : uint8_t
        {
            CreateRendererCore,
            DispatchFrameInfrastructure,
            WaitFrameInfrastructure,
            FinalizeCore,
            Complete
        };

        void PumpCoreInit();
        void PumpLoading();
        void PumpBackground();
        void EnterFailedState(const char* what);
        void UpdateWindowStatusText();

        ApplicationCore& m_app;
        InitTaskScheduler m_scheduler;
        MainThreadTaskPump m_pump;
        BootPhase m_phase = BootPhase::CreatingRendererCore;
        CoreStep m_coreStep = CoreStep::CreateRendererCore;
        InitTaskScheduler::TaskId m_frameInfraTask = InitTaskScheduler::kInvalidTask;
        bool m_failed = false;
        bool m_gameInitInvoked = false;
        bool m_pendingResize = false;
        UINT m_pendingResizeWidth = 0;
        UINT m_pendingResizeHeight = 0;
    };
}
