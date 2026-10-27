#include "ImGuiCoordinator.h"

#include "Foundation/Tools/DebugOutput.h"

#include <imgui_internal.h>

#include <string>

namespace SasamiEngine::App
{
    namespace {
        // Dear ImGui's error recovery (1.91.5+) reports a mismatched Begin/End or Push/Pop through a
        // red tooltip by default. That tooltip is itself a window whose End() runs the recovery again,
        // so one leaked stack entry recursed until the stack overflowed (c00000fd). Route the
        // message to our log instead and keep the silent recovery.
        void LogImGuiError(ImGuiContext* ctx, void*, const char* msg)
        {
            // A leak repeats every frame; the first few lines are enough to find it.
            static int s_loggedErrors = 0;
            if (s_loggedErrors >= 32) {
                return;
            }
            ++s_loggedErrors;
            const ImGuiWindow* window = ctx ? ctx->CurrentWindow : nullptr;
            DebugLog((std::string("[ImGui] error in window '") + (window ? window->Name : "NULL") + "': " +
                      (msg ? msg : "") + "\n").c_str());
        }

        void InstallErrorLogging()
        {
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigErrorRecoveryEnableTooltip = false;
            io.ConfigErrorRecoveryEnableDebugLog = false;
            ImGuiContext& g = *ImGui::GetCurrentContext();
            g.ErrorCallback = &LogImGuiError;
            g.ErrorCallbackUserData = nullptr;
        }

        // Windows can be dragged into each other or onto the screen edges (docking branch).
        // Multi-viewport (windows outside the main HWND) stays off: every UIRenderBackend would
        // need the platform-window renderer hooks for it.
        void EnableDocking()
        {
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        }
    }

    ImGuiCoordinator& ImGuiCoordinator::Instance()
    {
        static ImGuiCoordinator s_instance;
        return s_instance;
    }

    bool ImGuiCoordinator::Initialize(HWND hWnd, const UI::UIRenderBackendInitDesc& desc)
    {
        if (m_initialized)
            return true;

        IMGUI_CHECKVERSION();
        // Enable per-monitor DPI awareness for proper mouse scaling on DPI changes
        ImGui_ImplWin32_EnableDpiAwareness();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        InstallErrorLogging();
        EnableDocking();
        if (!ImGui_ImplWin32_Init(hWnd))
            return false;

        m_backend = UI::CreateUIRenderBackend();
        if (m_backend && !m_backend->Initialize(desc)) {
            // The renderer backend failed to stand up (or the build has none). Fall back to
            // platform-only: input and window drawers still work, only rendering is skipped.
            m_backend.reset();
        }
        m_frameOverlay.SetBackend(m_backend.get());

        m_initialized = true;
        return true;
    }

    bool ImGuiCoordinator::InitializePlatformOnly(HWND hWnd)
    {
        if (m_initialized)
            return true;

        IMGUI_CHECKVERSION();
        ImGui_ImplWin32_EnableDpiAwareness();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        EnableDocking();
        if (!ImGui_ImplWin32_Init(hWnd))
            return false;

        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        m_initialized = true;
        return true;
    }

    void ImGuiCoordinator::Shutdown()
    {
        if (!m_initialized) return;
        if (m_backend) {
            m_backend->Shutdown();
            m_backend.reset();
        }
        m_frameOverlay.SetBackend(nullptr);
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_panels.Clear();
        m_overlays.clear();
        m_initialized = false;
    }

    void ImGuiCoordinator::NewFrame()
    {
        if (!m_initialized) return;
        if (m_backend) {
            m_backend->NewFrame();
        }
        // A frame that ran NewFrame but never reached RenderFrame (e.g. the frame a scene is
        // replaced) left ImGui's frame open. NewFrame on top of it leaked one font-stack entry per
        // occurrence ("Forgot to call Render() or EndFrame()" in Debug); close it without drawing.
        if (!m_frameEnded) {
            ImGui::EndFrame();
            m_frameEnded = true;
        }
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // A dock space over the whole client area. The central node is passthru, so the scene
        // stays visible and clicks on it still reach the app (WantCaptureMouse stays false).
        // Nothing is drawn until a window is docked.
        if (m_dockSpaceEnabled) {
            m_dockSpaceId = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
        }

        m_panels.Draw();

        // After the windows, so an overlay can sit visually on top of them, and outside the
        // Begin/End pair so a collapsed window cannot suppress it.
        for (auto& overlay : m_overlays)
        {
            if (overlay) overlay();
        }

        m_frameEnded = false;
    }

    void ImGuiCoordinator::EndFrame()
    {
        if (!m_initialized || m_frameEnded) return;
        // This is the only place ImGui::Render() is called. Draw data has to exist before the
        // backend's own frame runs: on D3D12 the app hands the overlay a command list later in
        // the same frame, but every other backend records the overlay from inside its own
        // Present-bound frame (see IRhiFrameOverlay), which can happen before control ever
        // returns to ApplicationCore's render call.
        ImGui::Render();
        m_frameEnded = true;
    }

    void ImGuiCoordinator::RenderDrawData(const Render::RhiOverlayFrameContext& context)
    {
        if (!m_initialized || !m_backend || !m_drawVisible) return;
        m_backend->RenderDrawData(context);
    }

    void ImGuiCoordinator::SetDrawVisible(bool visible)
    {
        m_drawVisible = visible;
        m_frameOverlay.SetVisible(visible);
    }

    Render::IRhiFrameOverlay* ImGuiCoordinator::GetFrameOverlay()
    {
        return &m_frameOverlay;
    }

    void ImGuiCoordinator::RegisterWindow(const char* name, std::function<void()> drawFn)
    {
        m_panels.AddWindow(name ? name : "Window", "App", std::move(drawFn));
    }

    void ImGuiCoordinator::RegisterTab(const char* windowName, const char* category, const char* tabLabel,
                                       std::function<void()> drawFn)
    {
        m_panels.AddTab(windowName ? windowName : "Window", category ? category : "App",
                        tabLabel ? tabLabel : "Tab", std::move(drawFn));
    }

    void ImGuiCoordinator::RegisterOverlay(std::function<void()> drawFn)
    {
        m_overlays.push_back(std::move(drawFn));
    }

    bool ImGuiCoordinator::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!m_initialized) return false;
        return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }

    bool ImGuiCoordinator::WantsMouseCapture() const
    {
        return m_initialized ? ImGui::GetIO().WantCaptureMouse : false;
    }

    bool ImGuiCoordinator::WantsKeyboardCapture() const
    {
        return m_initialized ? ImGui::GetIO().WantCaptureKeyboard : false;
    }
}
