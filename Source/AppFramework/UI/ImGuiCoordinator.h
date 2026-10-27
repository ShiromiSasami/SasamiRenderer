#pragma once
#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <windows.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"

#include "Backend/IUIRenderBackend.h"
#include "Backend/UIFrameOverlay.h"
#include "UIPanelRegistry.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace SasamiEngine::App
{
    // Owns the ImGui context and the Win32 platform backend (Win32 is identical across every
    // graphics backend, so it lives here rather than in IUIRenderBackend). The renderer-specific
    // half is delegated to whichever UI::IUIRenderBackend the build compiles in.
    class ImGuiCoordinator
    {
    public:
        static ImGuiCoordinator& Instance();

        bool InitializePlatformOnly(HWND hWnd);
        bool Initialize(HWND hWnd, const UI::UIRenderBackendInitDesc& desc);
        void Shutdown();
        void NewFrame();
        void EndFrame();
        void RenderDrawData(const Render::RhiOverlayFrameContext& context);

        // Hides the UI from the rendered image without tearing ImGui down: the frame is still
        // built (so input and window state keep working), only its draw data is not recorded.
        // Used by deterministic captures, where the panel would otherwise be part of the image.
        void SetDrawVisible(bool visible);
        bool IsDrawVisible() const { return m_drawVisible; }

        // Main-viewport dock space (on by default). Off leaves docking between windows possible.
        void SetDockSpaceEnabled(bool enabled) { m_dockSpaceEnabled = enabled; }
        bool IsDockSpaceEnabled() const { return m_dockSpaceEnabled; }
        // Id of this frame's main dock space node (0 while it is disabled), for DockBuilder layouts.
        ImGuiID GetDockSpaceId() const { return m_dockSpaceEnabled ? m_dockSpaceId : 0; }

        // Lets ApplicationCore hand this frame's overlay recorder to the renderer. Owned here,
        // only borrowed by the renderer.
        Render::IRhiFrameOverlay* GetFrameOverlay();

        // Register UI drawers (thread-unsafe, call during init)
        // A window in category "App" of the panel registry (the editor's Window menu lists it).
        void RegisterWindow(const char* name, std::function<void()> drawFn);
        // Appends a tab to window `windowName`; a second tab brings in a tab bar automatically.
        void RegisterTab(const char* windowName, const char* category, const char* tabLabel,
                         std::function<void()> drawFn);
        UI::UIPanelRegistry& Panels() { return m_panels; }

        // Register a drawer that runs every frame regardless of any window's state.
        //
        // A window's callback only runs while ImGui::Begin returns true, which is false when the
        // user collapses it. Viewport overlays drawn through the background/foreground draw list
        // therefore vanish when an unrelated panel is collapsed -- the light gizmo did exactly
        // that, because its drawing lived inside the "PBR Controls" callback. Overlays belong
        // here instead, so their own visibility flag is the only thing that controls them.
        void RegisterOverlay(std::function<void()> drawFn);

        // Feed a Win32 message to ImGui. Returns true if ImGui handled it.
        bool HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        bool WantsMouseCapture() const;
        bool WantsKeyboardCapture() const;

    private:
        ImGuiCoordinator() = default;
        ~ImGuiCoordinator() = default;
        ImGuiCoordinator(const ImGuiCoordinator&) = delete;
        ImGuiCoordinator& operator=(const ImGuiCoordinator&) = delete;

        // Data
        std::unique_ptr<UI::IUIRenderBackend> m_backend;
        UI::UIFrameOverlay m_frameOverlay;
        UI::UIPanelRegistry m_panels;
        std::vector<std::function<void()>> m_overlays;
        bool m_initialized = false;
        bool m_drawVisible = true;
        bool m_dockSpaceEnabled = true;
        ImGuiID m_dockSpaceId = 0;
        // Guards against ImGui::Render() running twice in one frame -- EndFrame() can be called
        // from either the normal frame path or the boot loading frame path.
        bool m_frameEnded = true;
    };
}
namespace SasamiEngine { using namespace App; }
