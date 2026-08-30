#pragma once

#include <exception>
#include <fstream>
#include <string>
#include <windows.h>

#include "Foundation/Tools/DebugLogSystem.h"

namespace SasamiRenderer
{
    inline std::wstring NarrowToWideBestEffort(const char* text)
    {
        if (!text || *text == '\0') {
            return L"";
        }

        const int utf8Required = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (utf8Required > 0) {
            std::wstring wide(static_cast<size_t>(utf8Required), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), utf8Required);
            if (!wide.empty() && wide.back() == L'\0') {
                wide.pop_back();
            }
            return wide;
        }

        const int acpRequired = ::MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
        if (acpRequired > 0) {
            std::wstring wide(static_cast<size_t>(acpRequired), L'\0');
            ::MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), acpRequired);
            if (!wide.empty() && wide.back() == L'\0') {
                wide.pop_back();
            }
            return wide;
        }

        return L"(failed to convert exception message to wide string)";
    }

    inline void DebugLog(const char* message)
    {
        if (!message) {
            return;
        }
        DebugLogSystem::Instance().Log(DebugLogLevel::Info, message);
    }

    inline void DebugLog(const wchar_t* message)
    {
        if (!message) {
            return;
        }
        DebugLogSystem::Instance().Log(DebugLogLevel::Info, message);
    }

    inline int DebugLogDialog(const wchar_t* message,
                              const wchar_t* title = L"SasamiRenderer Debug",
                              UINT flags = MB_OK | MB_ICONINFORMATION)
    {
        if (!message) {
            return 0;
        }

        DebugLog(message);
        const wchar_t* dialogTitle = (title && *title) ? title : L"SasamiRenderer Debug";
        return ::MessageBoxW(nullptr, message, dialogTitle, flags);
    }

    inline int DebugLogDialog(const char* message,
                              const wchar_t* title = L"SasamiRenderer Debug",
                              UINT flags = MB_OK | MB_ICONINFORMATION)
    {
        if (!message) {
            return 0;
        }

        DebugLog(message);
        const std::wstring wideMessage = NarrowToWideBestEffort(message);
        const wchar_t* dialogTitle = (title && *title) ? title : L"SasamiRenderer Debug";
        return ::MessageBoxW(nullptr, wideMessage.c_str(), dialogTitle, flags);
    }

    inline void ReportException(const wchar_t* context, const std::exception& ex, bool showDialog)
    {
        std::wstring fullMessage = L"[Exception] ";
        if (context && *context) {
            fullMessage += context;
            fullMessage += L": ";
        }
        fullMessage += NarrowToWideBestEffort(ex.what());
        fullMessage += L"\n";

        DebugLog(fullMessage.c_str());

        if (showDialog) {
            ::MessageBoxW(nullptr, fullMessage.c_str(), L"SasamiRenderer Exception", MB_OK | MB_ICONERROR);
        }
    }

    inline void ReportUnknownException(const wchar_t* context, bool showDialog)
    {
        std::wstring fullMessage = L"[Exception] ";
        if (context && *context) {
            fullMessage += context;
            fullMessage += L": ";
        }
        fullMessage += L"Unknown exception.\n";

        DebugLog(fullMessage.c_str());

        if (showDialog) {
            ::MessageBoxW(nullptr, fullMessage.c_str(), L"SasamiRenderer Exception", MB_OK | MB_ICONERROR);
        }
    }

#if defined(_DEBUG)
    namespace Internal
    {
        inline unsigned int& GetDebugDrawCounter()
        {
            static unsigned int sCount = 0;
            return sCount;
        }
    }

    // Running count of Draw/DrawIndexed calls issued this frame. Lets RenderGraph
    // correlate a D3D12 GPU-based-validation "Draw Index: [N]" report back to the
    // render pass that issued it, without a PIX/RenderDoc GPU capture attached.
    inline void DebugResetDrawCount() { Internal::GetDebugDrawCounter() = 0; }
    inline void DebugIncrementDrawCount() { ++Internal::GetDebugDrawCounter(); }
    inline unsigned int DebugGetDrawCount() { return Internal::GetDebugDrawCounter(); }

    namespace Internal
    {
        inline unsigned int& GetDebugDispatchCounter()
        {
            static unsigned int sCount = 0;
            return sCount;
        }
    }

    // Running count of Dispatch calls issued this frame. Same purpose as the Draw
    // counter above, but for compute dispatches — correlates a GPU-based-validation
    // "Dispatch Index: [N]" report back to the compute pass that issued it.
    inline void DebugResetDispatchCount() { Internal::GetDebugDispatchCounter() = 0; }
    inline void DebugIncrementDispatchCount() { ++Internal::GetDebugDispatchCounter(); }
    inline unsigned int DebugGetDispatchCount() { return Internal::GetDebugDispatchCounter(); }
#endif
}
