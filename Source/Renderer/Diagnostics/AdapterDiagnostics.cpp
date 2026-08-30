// AdapterDiagnostics.cpp
// Startup diagnostic: logs the selected GPU adapter's name and VRAM
// usage/budget so another process silently hogging VRAM shows up immediately.
#include "Renderer/Diagnostics/AdapterDiagnostics.h"

#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    namespace
    {
        std::string ToUtf8(const WCHAR* wide)
        {
            if (!wide || wide[0] == L'\0') {
                return std::string();
            }

            const int required = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                return std::string();
            }

            std::string utf8(static_cast<size_t>(required), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), required, nullptr, nullptr);
            if (!utf8.empty() && utf8.back() == '\0') {
                utf8.pop_back();
            }
            return utf8;
        }
    }

    void LogAdapterDiagnostics(IDXGIAdapter1* adapter)
    {
        if (!adapter) {
            return;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            return;
        }

        const std::string name = ToUtf8(desc.Description);
        const unsigned long long dedicatedMiB =
            static_cast<unsigned long long>(desc.DedicatedVideoMemory) / (1024ull * 1024ull);

        char msg[256] = {};
        std::snprintf(msg, sizeof(msg), "[Adapter] %s (dedicated VRAM: %llu MiB)\n",
                      name.c_str(), dedicatedMiB);
        DebugLog(msg);

        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            return;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            return;
        }

        const unsigned long long usageMiB =
            static_cast<unsigned long long>(info.CurrentUsage) / (1024ull * 1024ull);
        const unsigned long long budgetMiB =
            static_cast<unsigned long long>(info.Budget) / (1024ull * 1024ull);

        char vramMsg[256] = {};
        std::snprintf(vramMsg, sizeof(vramMsg), "[Adapter] VRAM usage: %llu MiB / budget: %llu MiB\n",
                      usageMiB, budgetMiB);
        DebugLog(vramMsg);

        // Another process hogging VRAM does NOT show up as a high CurrentUsage: QueryVideoMemoryInfo
        // reports THIS process's usage. External pressure shows up as a shrunken Budget instead --
        // the OS lowers what this process is allowed to reside. At startup our own usage is still
        // near zero, so Budget vs. the adapter's physical VRAM is the only signal available here,
        // and it is the one that would have caught the 9.3 GB resident server that previously cost
        // us ~15 fps without any visible symptom.
        if (desc.DedicatedVideoMemory > 0 &&
            static_cast<double>(info.Budget) < static_cast<double>(desc.DedicatedVideoMemory) * 0.5) {
            char warnMsg[320] = {};
            std::snprintf(warnMsg, sizeof(warnMsg),
                          "[Adapter] 警告: VRAMバジェット(%llu MiB)が搭載量(%llu MiB)の半分未満です。"
                          "他プロセスがVRAMを占有している可能性が高く、フレームレートが大きく低下します。\n",
                          budgetMiB, dedicatedMiB);
            DebugLog(warnMsg);
        }

        // Separately, our own usage approaching the granted budget means eviction is imminent.
        if (info.Budget > 0 && static_cast<double>(info.CurrentUsage) > static_cast<double>(info.Budget) * 0.8) {
            char warnMsg[320] = {};
            std::snprintf(warnMsg, sizeof(warnMsg),
                          "[Adapter] 警告: 自プロセスのVRAM使用量(%llu MiB)がバジェット(%llu MiB)の80%%を超えています。\n",
                          usageMiB, budgetMiB);
            DebugLog(warnMsg);
        }
    }
}
