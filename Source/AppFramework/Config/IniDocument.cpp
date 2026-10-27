// Parsing half of IniDocument: turns the settings file's text into sections of key/value
// strings (via ParseIniStream) and hands out typed values. No knowledge of the renderer or of
// any sample.

#include "IniDocument.h"

#include "IniStreamParser.h"

#include <fstream>

namespace SasamiEngine::App
{
    bool IniDocument::LoadFromFile(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open()) {
            return false;
        }

        ParseIniStream(in, [this](const std::string& section, const IniKeyValues& entries) {
            for (const auto& [key, value] : entries) {
                m_sections[section][key] = value;
            }
        });
        return true;
    }

    const std::string* IniDocument::Find(const std::string& section, const char* key) const
    {
        const auto si = m_sections.find(section);
        if (si == m_sections.end()) return nullptr;
        const auto ki = si->second.find(key);
        return (ki == si->second.end()) ? nullptr : &ki->second;
    }

    float IniDocument::GetFloat(const std::string& section, const char* key, float fallback) const
    {
        const std::string* value = Find(section, key);
        if (!value) return fallback;
        try { return std::stof(*value); } catch (...) { return fallback; }
    }

    long IniDocument::GetLong(const std::string& section, const char* key, long fallback) const
    {
        const std::string* value = Find(section, key);
        if (!value) return fallback;
        try { return std::stol(*value); } catch (...) { return fallback; }
    }

    bool IniDocument::GetBool(const std::string& section, const char* key, bool fallback) const
    {
        return GetLong(section, key, fallback ? 1 : 0) != 0;
    }

    bool IniDocument::Has(const std::string& section, const char* key) const
    {
        return Find(section, key) != nullptr;
    }

    void IniDocument::SetValue(const std::string& section, const char* key, const std::string& value)
    {
        m_sections[section][key] = value;
    }

    void IniDocument::RenameKey(const std::string& section, const char* from, const char* to)
    {
        const auto si = m_sections.find(section);
        if (si == m_sections.end()) return;
        const auto ki = si->second.find(from);
        if (ki == si->second.end()) return;
        // A file that already carries the new key wins: it was written by a newer version than
        // the one this migration is undoing, so its value is the more trustworthy of the two.
        si->second.emplace(to, ki->second);
        si->second.erase(ki);
    }
}
