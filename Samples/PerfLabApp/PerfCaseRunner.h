#pragma once

#include "PerfCase.h"
#include "PerfStats.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    struct PerfVariantResult
    {
        PerfMetrics cold;                 // first run of the variant
        std::vector<PerfMetrics> warm;    // the `repeat` runs after it
        PerfSummary cpuMs;                // over the warm runs
        PerfSummary gpuMs;                // over the warm runs that measured the GPU
        uint32_t gpuExcluded = 0;         // warm runs left out of gpuMs for a worse "pstate" extra
    };

    struct PerfCaseResult
    {
        std::string caseName;
        uint32_t repeat = 0;
        bool prepared = false;
        bool checkOk = false;
        std::string detail;               // Prepare error or Check detail
        PerfVariantResult variants[2];    // indexed by PerfVariant
    };

    // Runs perf cases one variant run per Tick (one Tick per frame), alternating baseline and
    // optimised so slow drift (clock boost, caches of other work) hits both alike.
    class PerfCaseRunner
    {
    public:
        void AddCase(std::unique_ptr<IPerfCase> perfCase);
        const std::vector<std::unique_ptr<IPerfCase>>& Cases() const { return m_cases; }
        IPerfCase* FindCase(const std::string& name) const;

        // Queues a case; false if the name is unknown. repeat = warm runs per variant (>= 1).
        bool Enqueue(const std::string& name, uint32_t repeat);
        void EnqueueAll(uint32_t repeat);

        // Advances the running case by one variant run, starting the next queued case if idle.
        // Returns true when a case finished in this call.
        bool Tick();

        bool IsBusy() const { return m_active != nullptr || !m_queue.empty(); }
        const std::string& ActiveName() const { return m_activeResult.caseName; }
        uint32_t ActiveStep() const { return m_step; }
        uint32_t ActiveTotal() const { return 2 * (m_activeResult.repeat + 1); }
        size_t QueuedCount() const { return m_queue.size(); }

        // Finished results, newest last; a re-run replaces the previous result of that case.
        const std::vector<PerfCaseResult>& Results() const { return m_results; }
        const PerfCaseResult* FindResult(const std::string& name) const;

    private:
        struct Pending
        {
            IPerfCase* perfCase = nullptr;
            uint32_t repeat = 1;
        };

        void Start(const Pending& pending);
        void Finish();

        std::vector<std::unique_ptr<IPerfCase>> m_cases;
        std::deque<Pending> m_queue;
        IPerfCase* m_active = nullptr;
        uint32_t m_step = 0;
        PerfCaseResult m_activeResult;
        std::vector<PerfCaseResult> m_results;
    };
}
