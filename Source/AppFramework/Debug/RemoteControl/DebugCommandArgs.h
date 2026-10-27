#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Argument helpers shared by the debug command handlers. Every parser takes the whole token:
// trailing characters ("1.5x") fail instead of being ignored.
namespace SasamiEngine::App::Debug
{
    inline std::optional<float> ParseFloat(const std::string& text)
    {
        try {
            size_t consumed = 0;
            const float value = std::stof(text, &consumed);
            if (consumed != text.size()) {
                return std::nullopt;
            }
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }

    // Finite and > 0.
    inline std::optional<float> ParsePositiveFloat(const std::string& text)
    {
        const std::optional<float> value = ParseFloat(text);
        if (!value || !std::isfinite(*value) || *value <= 0.0f) {
            return std::nullopt;
        }
        return value;
    }

    inline std::optional<int> ParseInt(const std::string& text)
    {
        try {
            size_t consumed = 0;
            const int value = std::stoi(text, &consumed);
            if (consumed != text.size()) {
                return std::nullopt;
            }
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }

    inline std::optional<uint32_t> ParseUintInRange(const std::string& text, uint32_t minValue, uint32_t maxValue)
    {
        try {
            size_t consumed = 0;
            const long long value = std::stoll(text, &consumed);
            if (consumed != text.size() || value < static_cast<long long>(minValue) || value > static_cast<long long>(maxValue)) {
                return std::nullopt;
            }
            return static_cast<uint32_t>(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    inline std::optional<bool> ParseOnOff(const std::string& text)
    {
        if (text == "on") {
            return true;
        }
        if (text == "off") {
            return false;
        }
        return std::nullopt;
    }

    // "x,y,z": exactly three finite floats.
    inline bool ParseFloat3(const std::string& text, float out[3])
    {
        std::istringstream in(text);
        std::string part;
        for (int k = 0; k < 3; ++k) {
            if (!std::getline(in, part, ',')) {
                return false;
            }
            const std::optional<float> value = ParseFloat(part);
            if (!value || !std::isfinite(*value)) {
                return false;
            }
            out[k] = *value;
        }
        return !std::getline(in, part, ',');
    }

    // key=value tokens; false on a token without '=' or a key outside `allowed`.
    inline bool ParseKeyValues(const std::vector<std::string>& args, const std::vector<std::string>& allowed,
                               std::map<std::string, std::string>& out)
    {
        for (const std::string& arg : args) {
            const size_t eq = arg.find('=');
            if (eq == std::string::npos || eq == 0) {
                return false;
            }
            const std::string key = arg.substr(0, eq);
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
                return false;
            }
            out[key] = arg.substr(eq + 1);
        }
        return true;
    }

    inline std::string FormatFloat(float value)
    {
        std::string text = std::to_string(value);
        // Trim trailing zeros so "1.000000" reads as "1".
        const size_t dot = text.find('.');
        if (dot != std::string::npos) {
            size_t last = text.find_last_not_of('0');
            if (last == dot) {
                --last;
            }
            text.erase(last + 1);
        }
        return text;
    }

    inline std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // Joins the remaining tokens back with single spaces, so a path may contain spaces
    // even though the registry splits purely on whitespace.
    inline std::string JoinFrom(const std::vector<std::string>& args, size_t first)
    {
        std::string joined;
        for (size_t i = first; i < args.size(); ++i) {
            if (!joined.empty()) {
                joined += ' ';
            }
            joined += args[i];
        }
        return joined;
    }
}
