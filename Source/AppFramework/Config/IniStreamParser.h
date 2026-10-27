#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>

namespace SasamiEngine::App
{
    using IniKeyValues = std::unordered_map<std::string, std::string>;

    // Tokenizer shared by the scene file (ApplicationCoreScene.cpp) and IniDocument: lines are
    // trimmed, blank lines and lines starting with '#' are skipped, "[name]" opens a section and
    // "key = value" adds an entry (key and value trimmed, later duplicates win).
    // `onSection(name, entries)` runs at every section boundary and once at EOF, in file order,
    // so a section name that repeats ([static_model], [instance], ...) arrives once per block.
    // The first call carries the entries before any header, under the name "".
    void ParseIniStream(std::istream& in,
                        const std::function<void(const std::string&, const IniKeyValues&)>& onSection);
}
