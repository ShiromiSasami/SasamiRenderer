#include "GpuClockProbe.h"

#include <windows.h>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        // nvml.h: NVML_SUCCESS = 0, nvmlClockType_t NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_MEM = 2,
        // nvmlPstates_t NVML_PSTATE_UNKNOWN = 32.
        constexpr int kNvmlSuccess = 0;
        constexpr int kClockGraphics = 0;
        constexpr int kClockMemory = 2;
        constexpr int kPStateUnknown = 32;
    }

    GpuClockProbe::GpuClockProbe()
    {
        HMODULE library = LoadLibraryW(L"nvml.dll");
        if (!library) {
            return;
        }
        auto init = reinterpret_cast<InitFn>(GetProcAddress(library, "nvmlInit_v2"));
        auto shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(library, "nvmlShutdown"));
        auto getHandle = reinterpret_cast<GetHandleFn>(GetProcAddress(library, "nvmlDeviceGetHandleByIndex_v2"));
        auto getClock = reinterpret_cast<GetClockFn>(GetProcAddress(library, "nvmlDeviceGetClockInfo"));
        auto getPState = reinterpret_cast<GetPStateFn>(GetProcAddress(library, "nvmlDeviceGetPerformanceState"));
        if (!init || !shutdown || !getHandle || !getClock || !getPState || init() != kNvmlSuccess) {
            FreeLibrary(library);
            return;
        }
        // Device 0 is assumed to be the adapter the renderer runs on (not checked against the
        // D3D12 adapter LUID).
        void* device = nullptr;
        if (getHandle(0, &device) != kNvmlSuccess || !device) {
            shutdown();
            FreeLibrary(library);
            return;
        }
        m_library = library;
        m_device = device;
        m_shutdown = shutdown;
        m_getClock = getClock;
        m_getPState = getPState;
    }

    GpuClockProbe::~GpuClockProbe()
    {
        if (m_library) {
            m_shutdown();
            FreeLibrary(static_cast<HMODULE>(m_library));
        }
    }

    bool GpuClockProbe::Sample(GpuClockSample& out) const
    {
        out = GpuClockSample{};
        if (!m_device) {
            return false;
        }
        unsigned int graphics = 0;
        unsigned int memory = 0;
        int pstate = kPStateUnknown;
        if (m_getClock(m_device, kClockGraphics, &graphics) != kNvmlSuccess
            || m_getClock(m_device, kClockMemory, &memory) != kNvmlSuccess
            || m_getPState(m_device, &pstate) != kNvmlSuccess || pstate == kPStateUnknown) {
            return false;
        }
        out.graphicsMHz = static_cast<int>(graphics);
        out.memoryMHz = static_cast<int>(memory);
        out.pstate = pstate;
        return true;
    }
}
