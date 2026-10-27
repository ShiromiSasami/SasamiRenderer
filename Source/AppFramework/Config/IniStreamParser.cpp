#include "IniStreamParser.h"

#include <istream>

namespace SasamiEngine::App
{
    namespace
    {
        void Trim(std::string& value)
        {
            const auto begin = value.find_first_not_of(" \t\r\n");
            const auto end = value.find_last_not_of(" \t\r\n");
            value = (begin == std::string::npos) ? std::string{} : value.substr(begin, end - begin + 1);
        }
    }

    void ParseIniStream(std::istream& in,
                        const std::function<void(const std::string&, const IniKeyValues&)>& onSection)
    {
        std::string section;
        IniKeyValues entries;
        std::string line;
        while (std::getline(in, line)) {
            Trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '[') {
                onSection(section, entries);
                entries.clear();
                const auto close = line.find(']');
                section = (close != std::string::npos) ? line.substr(1, close - 1) : "";
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            Trim(key);
            Trim(val);
            if (!key.empty()) entries[key] = val;
        }
        onSection(section, entries);
    }
}
