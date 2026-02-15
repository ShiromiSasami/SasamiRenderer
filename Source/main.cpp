
#include <windows.h>
#include "ApplicationCore.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"

using namespace SasamiRenderer;

int WINAPI wmain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    ApplicationCore app(1280, 720, L"SasamiRenderer App");
    return app.Run();
}
