#pragma once

#include "GpuClockProbe.h"
#include "PerfCase.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    class ApplicationCore;
}

namespace SasamiEngine::Sample::PerfLab
{
    enum class CrowdCaseKind : uint32_t
    {
        Update, // Baseline = CrowdUpdateMode::Cpu,  Optimized = Gpu (both culled)
        Cull,   // Baseline = Gpu with culling off,  Optimized = Gpu with culling on
    };

    // 100k VAT crowd agents (Fox, Vertex mode) on the CrowdSystem of the renderer. Each variant run
    // switches the mode, waits the profiler latency plus kSettleFrames, then takes the median of
    // kSampleFrames frames: gpuMs is the frame total of every timed pass (as GBufferDrawCase),
    // cpuMs is the median wall time between frames (includes the CPU step in Cpu mode and any GPU
    // wait). extras: crowdScopeMs (the "Crowd_<n>" scope), visibleLod0..2 (read once at the end of
    // the run, blocking; -1 when culling is off), clocks/pstate from GpuClockProbe. DX12-only, like
    // the crowd itself.
    class CrowdCase final : public IPerfCase
    {
    public:
        CrowdCase(std::string name, App::ApplicationCore& app, CrowdCaseKind kind);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }
        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool StepVariant(PerfVariant variant, PerfMetrics& out) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        static constexpr uint32_t kAgentCount = 100000;
        static constexpr float kHalfExtent = 60.0f;
        static constexpr uint32_t kSettleFrames = 30;
        static constexpr uint32_t kSampleFrames = 60;

        bool ApplyVariant(PerfVariant variant, std::string& error);

        std::string m_name;
        std::string m_description;
        App::ApplicationCore& m_app;
        CrowdCaseKind m_kind;

        bool m_prepared = false;
        bool m_savedEnabled = false;
        uint32_t m_savedMode = 0;
        uint32_t m_savedStepMode = 0;
        bool m_savedCull = true;
        float m_savedHeight = 1.0f;
        std::unique_ptr<GpuClockProbe> m_clockProbe;

        bool m_running = false;
        uint32_t m_frame = 0;
        uint64_t m_lastTimingFrame = 0;
        std::chrono::steady_clock::time_point m_lastFrameTime{};
        std::vector<double> m_frameSamples;
        std::vector<double> m_crowdSamples;
        std::vector<double> m_wallSamples;
        std::vector<int> m_gpuClocks;
        std::vector<int> m_memClocks;
        int m_worstPState = -1;
        uint32_t m_runs[2] = {};
        uint32_t m_timedRuns[2] = {};
        std::string m_lastError;
    };
}
