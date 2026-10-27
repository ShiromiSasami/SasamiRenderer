#pragma once

#include <map>
#include <string>

namespace SasamiEngine::App
{
    // A parsed "[section] key = value" text file with typed lookups that fall back to a
    // caller-supplied default whenever an entry is missing or unparsable. IniWriter is the
    // counterpart that produces this syntax.
    //
    // Deliberately knows nothing about which sections or keys exist: that schema lives with the
    // caller (PBRApp's RenderingAppSettingsFile.cpp), so adding a persisted field never touches
    // the parser and changing the file syntax never touches the field list. Repeated sections are
    // merged; the scene reader, which needs each [static_model] block separately, uses
    // ParseIniStream (IniStreamParser.h) directly.
    class IniDocument
    {
    public:
        // False when the file could not be opened. The document is then empty and every
        // lookup yields its fallback, which is how a first run with no settings file behaves.
        bool LoadFromFile(const std::string& path);

        float GetFloat(const std::string& section, const char* key, float fallback) const;
        long GetLong(const std::string& section, const char* key, long fallback) const;
        bool GetBool(const std::string& section, const char* key, bool fallback) const;

        // The three edits a schema migration needs, so that a file written by an older version
        // can be rewritten in memory into what the current reader expects. Omitting a key is
        // not one of them: a sparse file already means "absent = today's default", so adding or
        // retiring a key needs no migration. What does need one is an existing key changing
        // meaning -- Has/SetValue cover a unit change or an enum renumber, RenameKey a rename.
        bool Has(const std::string& section, const char* key) const;
        void SetValue(const std::string& section, const char* key, const std::string& value);
        // No-op when `from` is absent; does not overwrite an existing `to`.
        void RenameKey(const std::string& section, const char* from, const char* to);

    private:
        const std::string* Find(const std::string& section, const char* key) const;

        std::map<std::string, std::map<std::string, std::string>> m_sections;
    };
}
