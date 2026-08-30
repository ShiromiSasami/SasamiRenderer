
#include <exception>
#include <windows.h>
#include "ApplicationCore.h"
#include "Foundation/Tools/DebugOutput.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"

using namespace SasamiRenderer;

namespace {
    // Records the exception code, faulting address and owning module in the app log.
    // Without this a crash only leaves a WER entry, which says nothing about which
    // subsystem (or which thread) actually faulted.
    LONG WINAPI LogUnhandledException(EXCEPTION_POINTERS* info)
    {
        if (!info || !info->ExceptionRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const void* address = info->ExceptionRecord->ExceptionAddress;
        wchar_t moduleName[MAX_PATH] = L"<unknown>";
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(address), &module) && module) {
            GetModuleFileNameW(module, moduleName, MAX_PATH);
        }

        char message[512];
        snprintf(message, sizeof(message),
                 "[Crash] unhandled exception code=0x%08lX flags=0x%lX address=%p thread=%lu module=%ls params=%llu",
                 info->ExceptionRecord->ExceptionCode,
                 info->ExceptionRecord->ExceptionFlags,
                 address,
                 GetCurrentThreadId(),
                 moduleName,
                 static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters));
        DebugLog(message);
        DebugLog("\n");

        for (DWORD i = 0; i < info->ExceptionRecord->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
            char param[128];
            snprintf(param, sizeof(param), "[Crash]   param[%lu]=0x%llX\n", i,
                     static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[i]));
            DebugLog(param);
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Vectored handler: runs on first chance, so it still reports failures that bypass
    // the unhandled filter entirely (__fastfail, heap corruption checks). Logs only
    // fatal codes to keep ordinary C++ exception unwinding out of the log.
    LONG WINAPI LogVectoredException(EXCEPTION_POINTERS* info)
    {
        if (!info || !info->ExceptionRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const DWORD code = info->ExceptionRecord->ExceptionCode;
        const bool fatal = (code == EXCEPTION_ACCESS_VIOLATION) ||
                           (code == EXCEPTION_STACK_OVERFLOW) ||
                           (code == EXCEPTION_ILLEGAL_INSTRUCTION) ||
                           (code == EXCEPTION_INT_DIVIDE_BY_ZERO) ||
                           (code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED) ||
                           (code == 0xC0000409u /* STATUS_STACK_BUFFER_OVERRUN / __fastfail */) ||
                           (code == 0xC0000374u /* STATUS_HEAP_CORRUPTION */) ||
                           (code == 0xE06D7363u /* MSVC C++ throw */) ||
                           (code == 0x0000087Du /* observed on this crash */);
        if (!fatal) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        LogUnhandledException(info);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

int wmain(int, wchar_t**) {
    AddVectoredExceptionHandler(1 /*first*/, &LogVectoredException);
    SetUnhandledExceptionFilter(&LogUnhandledException);
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
