#include "PerfCaseRunner.h"

#include <algorithm>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        // NVML P-state a case reported for the run (GBufferDrawCase via GpuClockProbe), -1 if none.
        int RunPState(const PerfMetrics& metrics)
        {
            for (const PerfExtraMetric& extra : metrics.extras) {
                if (extra.name == "pstate") {
                    return static_cast<int>(extra.value);
                }
            }
            return -1;
        }
    }

    void PerfCaseRunner::AddCase(std::unique_ptr<IPerfCase> perfCase)
    {
        if (perfCase) {
            m_cases.push_back(std::move(perfCase));
        }
    }

    IPerfCase* PerfCaseRunner::FindCase(const std::string& name) const
    {
        for (const auto& perfCase : m_cases) {
            if (name == perfCase->Name()) {
                return perfCase.get();
            }
        }
        return nullptr;
    }

    bool PerfCaseRunner::Enqueue(const std::string& name, uint32_t repeat)
    {
        IPerfCase* perfCase = FindCase(name);
        if (!perfCase) {
            return false;
        }
        m_queue.push_back({ perfCase, (std::max)(1u, repeat) });
        return true;
    }

    void PerfCaseRunner::EnqueueAll(uint32_t repeat)
    {
        for (const auto& perfCase : m_cases) {
            m_queue.push_back({ perfCase.get(), (std::max)(1u, repeat) });
        }
    }

    const PerfCaseResult* PerfCaseRunner::FindResult(const std::string& name) const
    {
        for (const PerfCaseResult& result : m_results) {
            if (result.caseName == name) {
                return &result;
            }
        }
        return nullptr;
    }

    void PerfCaseRunner::Start(const Pending& pending)
    {
        m_active = pending.perfCase;
        m_step = 0;
        m_activeResult = PerfCaseResult{};
        m_activeResult.caseName = m_active->Name();
        m_activeResult.repeat = pending.repeat;
        m_activeResult.prepared = m_active->Prepare(m_activeResult.detail);
    }

    bool PerfCaseRunner::Tick()
    {
        if (!m_active) {
            if (m_queue.empty()) {
                return false;
            }
            const Pending next = m_queue.front();
            m_queue.pop_front();
            Start(next);
            if (!m_activeResult.prepared) {
                Finish();
                return true;
            }
            // Prepare can be slow (file loads); the first run waits for the next frame.
            return false;
        }

        const PerfVariant variant = (m_step % 2 == 0) ? PerfVariant::Baseline : PerfVariant::Optimized;
        PerfVariantResult& out = m_activeResult.variants[static_cast<uint32_t>(variant)];
        PerfMetrics metrics;
        if (!m_active->StepVariant(variant, metrics)) {
            return false;
        }
        if (m_step < 2) {
            out.cold = metrics;
        } else {
            out.warm.push_back(metrics);
        }
        ++m_step;
        if (m_step < ActiveTotal()) {
            return false;
        }
        m_activeResult.checkOk = m_active->Check(m_activeResult.detail);
        Finish();
        return true;
    }

    void PerfCaseRunner::Finish()
    {
        // Warm runs taken in a worse P-state than the best one seen across both variants are left
        // out of gpuMs, so a clock drop does not pass for a regression. (The ~0.08 vs ~1.4 ms split
        // of the gbuffer_sponza GBuffer scope was not a P-state effect; see GBufferDrawCase.)
        // Runs without the extra are kept as before.
        int bestPState = -1;
        for (const PerfVariantResult& variant : m_activeResult.variants) {
            for (const PerfMetrics& m : variant.warm) {
                const int pstate = RunPState(m);
                if (pstate >= 0 && (bestPState < 0 || pstate < bestPState)) {
                    bestPState = pstate;
                }
            }
        }
        for (PerfVariantResult& variant : m_activeResult.variants) {
            std::vector<double> cpu;
            std::vector<double> gpu;
            variant.gpuExcluded = 0;
            for (const PerfMetrics& m : variant.warm) {
                cpu.push_back(m.cpuMs);
                if (m.gpuMs < 0.0) {
                    continue;
                }
                const int pstate = RunPState(m);
                if (pstate >= 0 && pstate != bestPState) {
                    ++variant.gpuExcluded;
                    continue;
                }
                gpu.push_back(m.gpuMs);
            }
            variant.cpuMs = Summarize(cpu);
            variant.gpuMs = Summarize(gpu);
        }
        m_active->Release();

        auto existing = std::find_if(m_results.begin(), m_results.end(),
            [this](const PerfCaseResult& r) { return r.caseName == m_activeResult.caseName; });
        if (existing != m_results.end()) {
            m_results.erase(existing);
        }
        m_results.push_back(std::move(m_activeResult));
        m_activeResult = PerfCaseResult{};
        m_active = nullptr;
        m_step = 0;
    }
}
