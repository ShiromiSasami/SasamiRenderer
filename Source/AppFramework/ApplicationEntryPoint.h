#pragma once

#include <cwctype>
#include <exception>
#include <optional>
#include <string_view>
#include <type_traits>
#include <windows.h>
#include "ApplicationCore.h"
#include "Foundation/Tools/DebugOutput.h"
#include "backends/imgui_impl_win32.h"

namespace SasamiRenderer
{
    inline wchar_t ToLowerAscii(wchar_t ch)
    {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    }

    inline bool EqualsIgnoreCase(std::wstring_view lhs, std::wstring_view rhs)
    {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (size_t i = 0; i < lhs.size(); ++i) {
            if (ToLowerAscii(lhs[i]) != ToLowerAscii(rhs[i])) {
                return false;
            }
        }
        return true;
    }

    inline std::optional<GraphicsRuntime> ParseGraphicsRuntimeName(std::wstring_view value)
    {
        if (EqualsIgnoreCase(value, L"dx12") ||
            EqualsIgnoreCase(value, L"d3d12") ||
            EqualsIgnoreCase(value, L"directx12")) {
            return GraphicsRuntime::DirectX12;
        }
        if (EqualsIgnoreCase(value, L"vulkan") ||
            EqualsIgnoreCase(value, L"vk")) {
            return GraphicsRuntime::Vulkan;
        }
        if (EqualsIgnoreCase(value, L"dx11") ||
            EqualsIgnoreCase(value, L"d3d11") ||
            EqualsIgnoreCase(value, L"directx11")) {
            return GraphicsRuntime::DirectX11;
        }
        if (EqualsIgnoreCase(value, L"opengl") ||
            EqualsIgnoreCase(value, L"gl")) {
            return GraphicsRuntime::OpenGL;
        }
        return std::nullopt;
    }

    inline std::optional<GraphicsRuntime> ParseGraphicsRuntimeArguments(int argc, wchar_t** argv)
    {
        for (int i = 1; i < argc; ++i) {
            if (!argv[i]) {
                continue;
            }

            const std::wstring_view arg(argv[i]);
            if (arg == L"--vulkan") {
                return GraphicsRuntime::Vulkan;
            }
            if (arg == L"--dx12" || arg == L"--d3d12") {
                return GraphicsRuntime::DirectX12;
            }
            if (arg == L"--dx11" || arg == L"--d3d11") {
                return GraphicsRuntime::DirectX11;
            }
            if (arg == L"--opengl") {
                return GraphicsRuntime::OpenGL;
            }

            constexpr std::wstring_view graphicsEquals = L"--graphics=";
            constexpr std::wstring_view backendEquals = L"--backend=";
            if (arg.rfind(graphicsEquals, 0) == 0) {
                return ParseGraphicsRuntimeName(arg.substr(graphicsEquals.size()));
            }
            if (arg.rfind(backendEquals, 0) == 0) {
                return ParseGraphicsRuntimeName(arg.substr(backendEquals.size()));
            }
            if ((arg == L"--graphics" || arg == L"--backend") && i + 1 < argc && argv[i + 1]) {
                return ParseGraphicsRuntimeName(argv[i + 1]);
            }
        }
        return std::nullopt;
    }

    template<typename TApplication>
    int RunApplication(UINT width, UINT height, const wchar_t* title, int argc = 0, wchar_t** argv = nullptr)
    {
        static_assert(std::is_base_of_v<IApplication, TApplication>, "TApplication must inherit from IApplication.");

        try {
            ImGui_ImplWin32_EnableDpiAwareness();
            TApplication appImpl;
            ApplicationCore app(width, height, title, &appImpl);
            if (auto runtime = ParseGraphicsRuntimeArguments(argc, argv)) {
                app.SetGraphicsRuntime(*runtime);
            }
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

#define SASAMI_IMPLEMENT_APPLICATION(AppType, Width, Height, Title)  \
    int wmain(int argc, wchar_t** argv)                              \
    {                                                                \
        return ::SasamiRenderer::RunApplication<AppType>(Width, Height, Title, argc, argv); \
    }
