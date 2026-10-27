#pragma once

#include "PerfCaseRunner.h"
#include "PerfCsvReader.h"

#include <string>

namespace SasamiEngine::Sample::PerfLab
{
    // One median against the previous run's. ratio = now / prev; 0 when either side was not measured.
    struct PerfDelta
    {
        double prev = -1.0;
        double now = -1.0;
        double ratio = 0.0;
        bool regressed = false; // now > prev * (1 + threshold)
        bool improved = false;  // now < prev / (1 + threshold)
    };

    struct PerfComparison
    {
        bool hasPrevious = false;
        std::string note;      // why there is no previous run
        double threshold = 0.1;
        PerfDelta cpu[2];      // baseline, optimized
        PerfDelta gpu[2];
        bool AnyRegressed() const;
    };

    // Compares the warm medians of result with the previous CSV's medians. A metric is skipped
    // (ratio 0) when either side is -1 or <= 0. threshold is a fraction (0.1 = 10% slower).
    PerfComparison ComparePerfResult(const PerfCsvMedians& previous, const PerfCaseResult& result, double threshold);
}
