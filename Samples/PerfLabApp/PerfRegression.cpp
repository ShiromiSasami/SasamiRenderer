#include "PerfRegression.h"

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        PerfDelta MakeDelta(double prev, double now, double threshold)
        {
            PerfDelta delta;
            delta.prev = prev;
            delta.now = now;
            if (prev <= 0.0 || now <= 0.0) {
                return delta;
            }
            delta.ratio = now / prev;
            delta.regressed = delta.ratio > 1.0 + threshold;
            delta.improved = delta.ratio < 1.0 / (1.0 + threshold);
            return delta;
        }
    }

    bool PerfComparison::AnyRegressed() const
    {
        for (uint32_t v = 0; v < 2; ++v) {
            if (cpu[v].regressed || gpu[v].regressed) {
                return true;
            }
        }
        return false;
    }

    PerfComparison ComparePerfResult(const PerfCsvMedians& previous, const PerfCaseResult& result, double threshold)
    {
        PerfComparison comparison;
        comparison.hasPrevious = true;
        comparison.threshold = threshold;
        for (uint32_t v = 0; v < 2; ++v) {
            const PerfVariantResult& r = result.variants[v];
            comparison.cpu[v] = MakeDelta(previous.cpuMs[v], r.cpuMs.count ? r.cpuMs.median : -1.0, threshold);
            comparison.gpu[v] = MakeDelta(previous.gpuMs[v], r.gpuMs.count ? r.gpuMs.median : -1.0, threshold);
        }
        return comparison;
    }
}
