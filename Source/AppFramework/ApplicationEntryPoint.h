#pragma once

#include <exception>
#include <type_traits>
#include <windows.h>
#include "ApplicationCore.h"
#include "Foundation/Tools/DebugOutput.h"
#include "backends/imgui_impl_win32.h"

namespace SasamiRenderer
{
    template<typename TApplication>
    int RunApplication(UINT width, UINT height, const wchar_t* title)
    {
        static_assert(std::is_base_of_v<IApplication, TApplication>, "TApplication must inherit from IApplication.");

        try {
            ImGui_ImplWin32_EnableDpiAwareness();
            TApplication appImpl;
            ApplicationCore app(width, height, title, &appImpl);
            return app.Run();
        } catch (const std::exception& ex) {
            ReportException(L"RunApplication", ex, true);
            return -1;
        } catch (...) {
            ReportUnknownException(L"RunApplication", true);
            return -1;
        }
    }
}

#define SASAMI_IMPLEMENT_APPLICATION(AppType, Width, Height, Title) \
    int WINAPI wmain(HINSTANCE, HINSTANCE, PWSTR, int)               \
    {                                                                \
        return ::SasamiRenderer::RunApplication<AppType>(Width, Height, Title); \
    }
