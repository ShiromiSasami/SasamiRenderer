
#include <windows.h>
#include "Application.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"

using namespace SasamiRenderer;

int WINAPI wmain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Enable per-monitor DPI awareness before any window is created to avoid OS scaling/virtualization
    ImGui_ImplWin32_EnableDpiAwareness();
    Application app(1280, 720, L"Sasami DX12 Renderer App");
    return app.Run();
}
