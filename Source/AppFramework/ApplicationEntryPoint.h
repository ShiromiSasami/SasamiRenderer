#pragma once

#include <type_traits>
#include <windows.h>
#include "ApplicationCore.h"
#include "backends/imgui_impl_win32.h"

namespace SasamiRenderer
{
    template<typename TApplication>
    int RunApplication(UINT width, UINT height, const wchar_t* title)
    {
        static_assert(std::is_base_of_v<IApplication, TApplication>, "TApplication must inherit from IApplication.");

        ImGui_ImplWin32_EnableDpiAwareness();
        TApplication appImpl;
        ApplicationCore app(width, height, title, &appImpl);
        return app.Run();
    }
}

#define SASAMI_IMPLEMENT_APPLICATION(AppType, Width, Height, Title) \
    int WINAPI wmain(HINSTANCE, HINSTANCE, PWSTR, int)               \
    {                                                                \
        return ::SasamiRenderer::RunApplication<AppType>(Width, Height, Title); \
    }
