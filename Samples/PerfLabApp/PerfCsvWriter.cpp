#include "PerfCsvWriter.h"

#include <cstdio>
#include <fstream>
#include <system_error>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        void WriteRow(std::ofstream& out, const PerfCaseResult& result, PerfVariant variant,
                      const char* kind, int iteration, const PerfMetrics& m)
        {
            char line[512] = {};
            std::snprintf(line, sizeof(line), "%s,%s,%s,%d,%.6f,%.6f,%llu,%llu,%llu,%.6f,%d",
                result.caseName.c_str(), PerfVariantName(variant), kind, iteration,
                m.cpuMs, m.gpuMs,
                static_cast<unsigned long long>(m.cpuBytes),
                static_cast<unsigned long long>(m.gpuBytes),
                static_cast<unsigned long long>(m.diskBytes),
                m.loadSec, result.checkOk ? 1 : 0);
            out << line;
            out << ',';
            for (size_t i = 0; i < m.extras.size(); ++i) {
                char extra[96] = {};
                std::snprintf(extra, sizeof(extra), "%s%s=%.6g", i ? ";" : "", m.extras[i].name.c_str(), m.extras[i].value);
                out << extra;
            }
            out << '\n';
        }

        PerfMetrics SummaryRow(const PerfVariantResult& variant, double PerfSummary::*field)
        {
            // Byte metrics do not vary between runs of a case; take them from the cold run.
            PerfMetrics m = variant.cold;
            m.cpuMs = variant.cpuMs.*field;
            m.gpuMs = variant.gpuMs.count > 0 ? variant.gpuMs.*field : -1.0;
            return m;
        }
    }

    bool WritePerfCsv(const std::filesystem::path& dir, const PerfCaseResult& result, std::string& error)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            error = "cannot create " + dir.string() + ": " + ec.message();
            return false;
        }
        const std::filesystem::path path = dir / (result.caseName + ".csv");
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            error = "cannot open " + path.string();
            return false;
        }
        out << "case,variant,kind,iteration,cpuMs,gpuMs,cpuBytes,gpuBytes,diskBytes,loadSec,checkOk,extras\n";
        for (uint32_t v = 0; v < 2; ++v) {
            const PerfVariant variant = static_cast<PerfVariant>(v);
            const PerfVariantResult& r = result.variants[v];
            WriteRow(out, result, variant, "cold", 0, r.cold);
            for (size_t i = 0; i < r.warm.size(); ++i) {
                WriteRow(out, result, variant, "warm", static_cast<int>(i + 1), r.warm[i]);
            }
            WriteRow(out, result, variant, "median", -1, SummaryRow(r, &PerfSummary::median));
            WriteRow(out, result, variant, "min", -1, SummaryRow(r, &PerfSummary::min));
            WriteRow(out, result, variant, "max", -1, SummaryRow(r, &PerfSummary::max));
        }
        if (!out) {
            error = "write failed: " + path.string();
            return false;
        }
        return true;
    }
}
