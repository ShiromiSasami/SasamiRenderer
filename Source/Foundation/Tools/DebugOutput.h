#pragma once

#include <exception>
#include <string>
#include <windows.h>

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
        ::OutputDebugStringA(message);
    }

    inline void DebugLog(const wchar_t* message)
    {
        if (!message) {
            return;
        }
        ::OutputDebugStringW(message);
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
}
