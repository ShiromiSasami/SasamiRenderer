#pragma once

#include <iosfwd>
#include <string>

namespace SasamiEngine::App
{
    // Writing half of the settings file, and the counterpart to IniDocument's parsing half:
    // emits "[section]" / "key = value" text but drops every entry whose value equals the
    // default the caller passes alongside it. The result is a *sparse* file that records only
    // what differs from the code's defaults, so a key that is absent means "whatever the
    // default is today" rather than "whatever this machine happened to save once".
    //
    // Knows nothing about which sections or keys exist -- that schema lives in
    // RenderingAppSettingsFile.cpp, the same split IniDocument.h describes.
    class IniWriter
    {
    public:
        explicit IniWriter(std::ostream& out) : m_out(out) {}

        // The header is emitted lazily, on the section's first surviving entry, so a section
        // whose values are all default leaves no trace in the file at all.
        void BeginSection(const char* name);

        void WriteFloat(const char* key, float value, float defaultValue);
        void WriteInt(const char* key, long value, long defaultValue);
        void WriteBool(const char* key, bool value, bool defaultValue);

        // Emitted whether or not it matches anything: for the schema version, which a future
        // migration can only act on if it is actually present in every file.
        void WriteAlwaysInt(const char* key, long value);

        // The shortest decimal form that reads back as the same float (std::to_chars without a
        // precision argument). The stream default of 6 significant digits is not round-trip
        // safe, which matters twice here: a saved value would drift on each reload, and a value
        // that is *really* the default could print differently from it and be kept forever.
        // Comparing the formatted text rather than the float is what closes the second hole.
        static std::string FormatFloat(float value);

    private:
        void Emit(const char* key, const std::string& value);

        std::ostream& m_out;
        std::string m_pendingSection;
        bool m_sectionOpen = false;
    };
}
