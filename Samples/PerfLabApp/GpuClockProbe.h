#pragma once

#include <cstdint>

namespace SasamiEngine::Sample::PerfLab
{
    struct GpuClockSample
    {
        int graphicsMHz = -1;
        int memoryMHz = -1;
        int pstate = -1; // NVML P-state, 0 = maximum performance, 15 = minimum
    };

    // Reads the current GPU clocks and P-state through NVML (nvml.dll, loaded at run time), so
    // PerfLab can tell a power-state change from a real GPU-time difference. NVIDIA-only debt:
    // other vendors get Available() == false and every value stays -1. D3D12's
    // SetStablePowerState would pin the clocks instead, but it needs Windows Developer Mode and
    // removes the device without it (learn.microsoft.com, ID3D12Device::SetStablePowerState).
    // The sample is taken on the CPU when it is called, not when the GPU ran the timed frame.
    class GpuClockProbe
    {
    public:
        GpuClockProbe();
        ~GpuClockProbe();
        GpuClockProbe(const GpuClockProbe&) = delete;
        GpuClockProbe& operator=(const GpuClockProbe&) = delete;

        bool Available() const { return m_device != nullptr; }
        // False (and -1 values) when NVML is missing or a query fails.
        bool Sample(GpuClockSample& out) const;

    private:
        using InitFn = int (*)();
        using ShutdownFn = int (*)();
        using GetHandleFn = int (*)(unsigned int, void**);
        using GetClockFn = int (*)(void*, int, unsigned int*);
        using GetPStateFn = int (*)(void*, int*);

        void* m_library = nullptr;
        void* m_device = nullptr;
        ShutdownFn m_shutdown = nullptr;
        GetClockFn m_getClock = nullptr;
        GetPStateFn m_getPState = nullptr;
    };
}
