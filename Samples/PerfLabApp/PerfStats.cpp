#include "PerfStats.h"

#include <algorithm>

namespace SasamiEngine::Sample::PerfLab
{
    PerfSummary Summarize(std::vector<double> samples)
    {
        PerfSummary summary;
        if (samples.empty()) {
            return summary;
        }
        std::sort(samples.begin(), samples.end());
        const size_t n = samples.size();
        summary.count = static_cast<uint32_t>(n);
        summary.min = samples.front();
        summary.max = samples.back();
        summary.median = (n % 2 == 1) ? samples[n / 2] : 0.5 * (samples[n / 2 - 1] + samples[n / 2]);
        return summary;
    }
}
