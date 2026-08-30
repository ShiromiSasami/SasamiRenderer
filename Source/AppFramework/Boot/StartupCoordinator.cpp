#include "Boot/StartupCoordinator.h"

#include "ApplicationCore.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Runtime/RendererReadyState.h"

#include <iterator>
#include <string>

namespace SasamiRenderer
{
    namespace
    {
        // Cached once so repeated title-bar progress updates don't stack the phase
        // text onto itself; StartupCoordinator's header layout is fixed, so this
        // can't live in a member (see AppFramework/Boot/StartupCoordinator.h).
        const std::wstring& CachedOriginalTitle(ApplicationCore& app)
        {
            static std::wstring title;
            static bool captured = false;
            if (!captured) {
                wchar_t buffer[256] = {};
                if (HWND hwnd = app.GetHwnd()) {
                    GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
                }
                title = buffer;
                captured = true;
            }
            return title;
        }
    }

    StartupCoordinator::StartupCoordinator(ApplicationCore& app)
        : m_app(app)
    {
    }

    void StartupCoordinator::Pump()
    {
        switch (m_phase) {
        case BootPhase::CreatingRendererCore:
            PumpCoreInit();
            break;
        case BootPhase::LoadingWithBootScreen:
            PumpLoading();
            break;
        case BootPhase::SceneReady:
            PumpBackground();
            break;
        case BootPhase::Running:
            break;
        }
    }

    void StartupCoordinator::PumpCoreInit()
    {
        switch (m_coreStep) {
        case CoreStep::CreateRendererCore:
            UpdateWindowStatusText();
            if (!m_app.BootCreateRendererCore()) {
                EnterFailedState("Renderer core initialization failed.");
                return;
            }
            m_coreStep = CoreStep::DispatchFrameInfrastructure;
            break;

        case CoreStep::DispatchFrameInfrastructure:
            UpdateWindowStatusText();
            if (m_app.BootCanRunFrameInfraOnWorker()) {
                m_frameInfraTask = m_scheduler.AddWorkerTask("Renderer.FrameInfrastructure",
                    [this]() { return m_app.BootInitializeFrameInfrastructure(); });
                m_coreStep = CoreStep::WaitFrameInfrastructure;
            } else {
                if (!m_app.BootInitializeFrameInfrastructure()) {
                    EnterFailedState("Renderer frame infrastructure initialization failed.");
                    return;
                }
                m_coreStep = CoreStep::FinalizeCore;
            }
            break;

        case CoreStep::WaitFrameInfrastructure:
            UpdateWindowStatusText();
            m_scheduler.Pump(m_pump);
            m_pump.Execute(kBootBudgetMs);
            if (m_scheduler.AnyFailed()) {
                EnterFailedState("Renderer frame infrastructure initialization failed.");
                return;
            }
            if (m_scheduler.AllDone()) {
                m_coreStep = CoreStep::FinalizeCore;
            }
            break;

        case CoreStep::FinalizeCore:
            UpdateWindowStatusText();
            if (!m_app.BootFinalizeRendererCore()) {
                EnterFailedState("Renderer core finalization failed.");
                return;
            }
            if (m_pendingResize) {
                m_app.ResizeRenderer(m_pendingResizeWidth, m_pendingResizeHeight);
                m_pendingResize = false;
            }
            m_app.BootRegisterDeferredTasks(m_scheduler);
            m_coreStep = CoreStep::Complete;
            m_phase = BootPhase::LoadingWithBootScreen;
            m_app.SetWindowStatusText(CachedOriginalTitle(m_app).c_str());
            break;

        case CoreStep::Complete:
            break;
        }
    }

    void StartupCoordinator::PumpLoading()
    {
        if (!m_gameInitInvoked) {
            // One boot frame on screen before the game's (still-synchronous) OnInit
            // runs, so the user sees loading UI instead of an unresponsive window.
            m_app.BootRenderLoadingFrame(GetBootStatusSnapshot());
            m_app.BootInvokeGameOnInit();
            m_gameInitInvoked = true;
            return;
        }

        m_scheduler.Pump(m_pump);
        m_pump.Execute(kBootBudgetMs);
        m_app.BootRenderLoadingFrame(GetBootStatusSnapshot());
        UpdateWindowStatusText();

        bool rasterReady = false;
        if (m_app.IsRendererReady()) {
            const RendererReadyState& readyState = m_app.GetRenderer().GetReadyState();
            rasterReady = readyState.IsFeatureReady(readyState.rasterReady);
        }
        if (rasterReady && m_gameInitInvoked) {
            m_phase = BootPhase::SceneReady;
            // Restore the plain title; remaining task progress shows in the compact
            // ImGui overlay, not the title bar.
            m_app.SetWindowStatusText(CachedOriginalTitle(m_app).c_str());
        }
    }

    void StartupCoordinator::PumpBackground()
    {
        m_scheduler.Pump(m_pump);
        m_pump.Execute(kSceneBudgetMs);
        if (m_scheduler.AllDone()) {
            DebugLog("[Boot] all init tasks complete\n");
            m_phase = BootPhase::Running;
        }
    }

    void StartupCoordinator::NotifyResize(UINT width, UINT height)
    {
        if (m_phase >= BootPhase::SceneReady) {
            // Scene is already up: ApplicationCore's normal WM_SIZE path handles it.
            return;
        }
        m_pendingResize = true;
        m_pendingResizeWidth = width;
        m_pendingResizeHeight = height;
    }

    BootStatus StartupCoordinator::GetBootStatusSnapshot() const
    {
        BootStatus status;
        status.phase = m_phase;
        m_scheduler.CollectStatus(status);
        return status;
    }

    void StartupCoordinator::Shutdown()
    {
        m_scheduler.WaitForOutstandingWork();
    }

    void StartupCoordinator::EnterFailedState(const char* what)
    {
        std::string message = "StartupCoordinator: ";
        message += (what ? what : "boot failed.");
        message += "\n";
        DebugLogDialog(message.c_str(), L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
        m_failed = true;
    }

    void StartupCoordinator::UpdateWindowStatusText()
    {
        const wchar_t* phaseText = L"initializing...";
        switch (m_phase) {
        case BootPhase::CreatingRendererCore:
            switch (m_coreStep) {
            case CoreStep::CreateRendererCore:
                phaseText = L"initializing renderer core...";
                break;
            case CoreStep::DispatchFrameInfrastructure:
            case CoreStep::WaitFrameInfrastructure:
                phaseText = L"initializing GPU frame resources...";
                break;
            case CoreStep::FinalizeCore:
            case CoreStep::Complete:
                phaseText = L"finalizing renderer core...";
                break;
            }
            break;
        case BootPhase::LoadingWithBootScreen:
            phaseText = L"loading...";
            break;
        case BootPhase::SceneReady:
        case BootPhase::Running:
            phaseText = L"ready";
            break;
        }

        const BootStatus status = GetBootStatusSnapshot();
        std::wstring text = CachedOriginalTitle(m_app);
        text += L" - ";
        text += phaseText;
        if (status.totalCount > 0) {
            text += L" (" + std::to_wstring(status.completedCount) + L"/" + std::to_wstring(status.totalCount) + L")";
        }
        // Pump() runs every loop iteration (~1000 Hz while booting); only touch the
        // window when the text actually changed.
        static std::wstring lastText;
        if (text != lastText) {
            lastText = text;
            m_app.SetWindowStatusText(text.c_str());
        }
    }
}
