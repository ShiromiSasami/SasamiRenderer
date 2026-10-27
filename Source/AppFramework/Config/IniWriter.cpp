// Formatting half of the settings file: turns typed values into "[section] key = value" text
// and omits the ones that match their default. No knowledge of the renderer or of this sample.

#include "IniWriter.h"

#include <charconv>
#include <ostream>
#include <system_error>

namespace SasamiEngine::App
{
    void IniWriter::BeginSection(const char* name)
    {
        m_pendingSection = name;
        m_sectionOpen = false;
    }

    std::string IniWriter::FormatFloat(float value)
    {
        // to_chars without a precision argument is specified to produce the shortest string
        // that recovers the value exactly, and is locale-independent, so the decimal point
        // always matches what IniDocument's std::stof expects.
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec != std::errc{}) {
            // Unreachable for a float in a 64-byte buffer; keeps the file parsable regardless.
            return std::string("0");
        }
        return std::string(buffer, result.ptr);
    }

    void IniWriter::Emit(const char* key, const std::string& value)
    {
        if (!m_sectionOpen && !m_pendingSection.empty()) {
            m_out << '[' << m_pendingSection << "]\n";
            m_sectionOpen = true;
        }
        m_out << key << " = " << value << '\n';
    }

    void IniWriter::WriteFloat(const char* key, float value, float defaultValue)
    {
        std::string text = FormatFloat(value);
        // Text comparison, not value comparison: what the file records is the text, so two
        // values that print the same are the same as far as a later load is concerned.
        if (text == FormatFloat(defaultValue)) {
            return;
        }
        Emit(key, text);
    }

    void IniWriter::WriteInt(const char* key, long value, long defaultValue)
    {
        if (value == defaultValue) {
            return;
        }
        Emit(key, std::to_string(value));
    }

    void IniWriter::WriteBool(const char* key, bool value, bool defaultValue)
    {
        if (value == defaultValue) {
            return;
        }
        Emit(key, value ? "1" : "0");
    }

    void IniWriter::WriteAlwaysInt(const char* key, long value)
    {
        Emit(key, std::to_string(value));
    }
}
