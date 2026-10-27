#include "PerfCsvReader.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        // Column order of WritePerfCsv: case,variant,kind,iteration,cpuMs,gpuMs,...
        constexpr size_t kVariantColumn = 1;
        constexpr size_t kKindColumn = 2;
        constexpr size_t kCpuMsColumn = 4;
        constexpr size_t kGpuMsColumn = 5;

        std::vector<std::string> SplitCsvLine(const std::string& line)
        {
            std::vector<std::string> fields;
            std::stringstream stream(line);
            std::string field;
            while (std::getline(stream, field, ',')) {
                fields.push_back(field);
            }
            return fields;
        }
    }

    bool ReadPerfCsvMedians(const std::filesystem::path& path, PerfCsvMedians& out, std::string& error)
    {
        out = PerfCsvMedians{};
        std::ifstream in(path);
        if (!in) {
            error = "no previous csv";
            return false;
        }
        bool found = false;
        std::string line;
        std::getline(in, line); // header
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::vector<std::string> fields = SplitCsvLine(line);
            if (fields.size() <= kGpuMsColumn || fields[kKindColumn] != "median") {
                continue;
            }
            int index = -1;
            if (fields[kVariantColumn] == "baseline") {
                index = 0;
            } else if (fields[kVariantColumn] == "optimized") {
                index = 1;
            }
            if (index < 0) {
                continue;
            }
            out.cpuMs[index] = std::strtod(fields[kCpuMsColumn].c_str(), nullptr);
            out.gpuMs[index] = std::strtod(fields[kGpuMsColumn].c_str(), nullptr);
            found = true;
        }
        if (!found) {
            error = "no median row in " + path.string();
        }
        return found;
    }
}
