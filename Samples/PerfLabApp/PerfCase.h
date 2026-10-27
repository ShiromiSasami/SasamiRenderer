#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    enum class PerfVariant : uint32_t
    {
        Baseline = 0,
        Optimized = 1,
    };

    inline const char* PerfVariantName(PerfVariant variant)
    {
        return variant == PerfVariant::Baseline ? "baseline" : "optimized";
    }

    // A case-specific number (e.g. ACMR) that is not a time or a byte count.
    struct PerfExtraMetric
    {
        std::string name;
        double value = 0.0;
    };

    // One measured run. A metric a case does not measure keeps its "not measured" value.
    struct PerfMetrics
    {
        double cpuMs = 0.0;       // wall time of the measured section on the CPU
        double gpuMs = -1.0;      // GPU timestamps, -1 = not measured
        uint64_t cpuBytes = 0;    // CPU memory the variant needs for its data
        uint64_t gpuBytes = 0;    // GPU memory the variant needs for its data
        uint64_t diskBytes = 0;   // bytes read from or written to disk / transferred
        double loadSec = -1.0;    // load time in seconds, -1 = not measured
        std::vector<PerfExtraMetric> extras;
    };

    // A perf case is a pair: the baseline and the optimised version of the same work.
    // The runner calls Prepare once, then RunVariant alternately (the first call of each
    // variant is the cold run), then Check, then Release. All calls happen on the main thread,
    // one StepVariant per frame, so a single call must stay well under the 5 s command timeout.
    // A case that needs several frames per run (GPU timestamps arrive a few frames late)
    // overrides StepVariant and returns false until its run is complete.
    class IPerfCase
    {
    public:
        virtual ~IPerfCase() = default;

        virtual const char* Name() const = 0;
        virtual const char* Description() const = 0;

        virtual bool Prepare(std::string& error) = 0;
        virtual PerfMetrics RunVariant(PerfVariant variant) = 0;
        // Called once per frame; true when `out` holds the finished run of this variant.
        virtual bool StepVariant(PerfVariant variant, PerfMetrics& out)
        {
            out = RunVariant(variant);
            return true;
        }
        // After both variants have run: do they produce the same result?
        virtual bool Check(std::string& detail) = 0;
        virtual void Release() {}
    };
}
