#pragma once

#include "PerfCaseRunner.h"

#include <filesystem>
#include <string>

namespace SasamiEngine::Sample::PerfLab
{
    // Writes <dir>/<case>.csv (overwritten): one row per run (kind cold/warm) and the median,
    // min and max rows of the warm runs. "-1" marks a metric the case did not measure.
    bool WritePerfCsv(const std::filesystem::path& dir, const PerfCaseResult& result, std::string& error);
}
