#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    struct PerfSummary
    {
        uint32_t count = 0;
        double median = 0.0;
        double min = 0.0;
        double max = 0.0;
    };

    // Median (mean of the two middle values for an even count), min and max. Empty -> count 0.
    PerfSummary Summarize(std::vector<double> samples);
}
