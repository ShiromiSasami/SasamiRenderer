#include "Foundation/Profiling/Profiler.h"
#include "Renderer/RHI/GraphicsDevice.h"

#define NOMINMAX
#include <TraceLoggingProvider.h>
#include <atomic>
#include <cstring>
#include <windows.h>

#if defined(__has_include)
#if __has_include("pix3.h")
#define SASAMI_HAS_PIX3 1
#include "pix3.h"
#endif
#endif

#ifndef SASAMI_HAS_PIX3
#define SASAMI_HAS_PIX3 0
#endif

#pragma comment(lib, "advapi32.lib")

TRACELOGGING_DEFINE_PROVIDER(
    g_sasamiProfilerProvider,
    "SasamiRenderer",
    (0x7f4d1fb4, 0x2cb4, 0x4c78, 0x9b, 0x2f, 0x40, 0x5c, 0x2b, 0x6e, 0x6d, 0x51));

namespace SasamiRenderer::Profiler
{
    namespace
    {
        std::atomic_uint32_t g_registrationCount{ 0 };

        const char* SafeName(const char* name)
        {
            return (name && *name) ? name : "Unnamed";
        }
    }

    void Initialize()
    {
        const uint32_t oldCount = g_registrationCount.fetch_add(1, std::memory_order_acq_rel);
        if (oldCount == 0) {
            TraceLoggingRegister(g_sasamiProfilerProvider);
        }
    }

    void Shutdown()
    {
        const uint32_t oldCount = g_registrationCount.load(std::memory_order_acquire);
        if (oldCount == 0) {
            return;
        }

        if (g_registrationCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            TraceLoggingUnregister(g_sasamiProfilerProvider);
        }
    }

    void BeginCpuEvent(const char* name)
    {
        TraceLoggingWrite(
            g_sasamiProfilerProvider,
            "CpuEventBegin",
            TraceLoggingString(SafeName(name), "Name"));
    }

    void EndCpuEvent(const char* name)
    {
        TraceLoggingWrite(
            g_sasamiProfilerProvider,
            "CpuEventEnd",
            TraceLoggingString(SafeName(name), "Name"));
    }

    void MarkCpuEvent(const char* name)
    {
        TraceLoggingWrite(
            g_sasamiProfilerProvider,
            "CpuMarker",
            TraceLoggingString(SafeName(name), "Name"));
    }

    void BeginGpuEvent(CommandList* cmdList, const char* name)
    {
        if (!cmdList || !cmdList->Get()) {
            return;
        }

#if SASAMI_HAS_PIX3
        PIXBeginEvent(cmdList->Get(), PIX_COLOR(80, 180, 255), "%s", SafeName(name));
#else
        (void)name;
#endif
    }

    void EndGpuEvent(CommandList* cmdList)
    {
        if (!cmdList || !cmdList->Get()) {
            return;
        }

#if SASAMI_HAS_PIX3
        PIXEndEvent(cmdList->Get());
#endif
    }

    void MarkGpuEvent(CommandList* cmdList, const char* name)
    {
        if (!cmdList || !cmdList->Get()) {
            return;
        }

#if SASAMI_HAS_PIX3
        PIXSetMarker(cmdList->Get(), PIX_COLOR(80, 180, 255), "%s", SafeName(name));
#else
        (void)name;
#endif
    }
}
