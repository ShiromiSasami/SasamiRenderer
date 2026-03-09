
#include <exception>
#include <windows.h>
#include "ApplicationCore.h"
#include "Foundation/Tools/DebugOutput.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"

using namespace SasamiRenderer;

int WINAPI wmain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    try {
        ImGui_ImplWin32_EnableDpiAwareness();
        ApplicationCore app(1280, 720, L"SasamiRenderer App");
        return app.Run();
    } catch (const std::exception& ex) {
        ReportException(L"wmain", ex, true);
        return -1;
    } catch (...) {
        ReportUnknownException(L"wmain", true);
        return -1;
    }
}
