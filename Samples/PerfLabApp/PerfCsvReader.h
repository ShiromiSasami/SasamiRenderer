#pragma once

#include <filesystem>
#include <string>

namespace SasamiEngine::Sample::PerfLab
{
    // Median rows of one case's CSV as WritePerfCsv left it (index 0 = baseline, 1 = optimized).
    // -1 marks a value the CSV did not have (gpuMs -1 = not measured, or no median row).
    struct PerfCsvMedians
    {
        double cpuMs[2] = { -1.0, -1.0 };
        double gpuMs[2] = { -1.0, -1.0 };
    };

    // Reads <path>. Returns false (with error) if the file is missing or has no median row.
    bool ReadPerfCsvMedians(const std::filesystem::path& path, PerfCsvMedians& out, std::string& error);
}
