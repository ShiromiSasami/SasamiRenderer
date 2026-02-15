#pragma once

#include <windows.h>

namespace SasamiRenderer
{
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
}
