#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SasamiRenderer
{
    enum class DebugLogLevel { Info = 0, Warning = 1, Error = 2 };

    class DebugLogSystem
    {
    public:
        using Sink = std::function<void(DebugLogLevel level, std::wstring_view message)>;
        using SinkHandle = size_t;

        static DebugLogSystem& Instance();

        // sink only invoked for messages >= minLevel
        SinkHandle AddSink(Sink sink, DebugLogLevel minLevel = DebugLogLevel::Info);
        void RemoveSink(SinkHandle handle);

        void Log(DebugLogLevel level, const char* message);
        void Log(DebugLogLevel level, const wchar_t* message);

        // Same as above, but tags the message with an explicit category (e.g. "Renderer").
        // Unspecified-category calls above still fall back to the default category for
        // filtering purposes, but never gain a printed category tag (keeps default output
        // byte-identical to before this feature existed).
        void Log(DebugLogLevel level, const char* message, std::string_view category);
        void Log(DebugLogLevel level, const wchar_t* message, std::string_view category);

        // Timestamps are off by default; output is unchanged unless explicitly enabled.
        void SetTimestampEnabled(bool enabled);
        bool IsTimestampEnabled() const;

        // Categories not passed to SetCategoryEnabled() are enabled by default.
        void SetCategoryEnabled(std::string_view category, bool enabled);
        bool IsCategoryEnabled(std::string_view category) const;

        // Rotation only ever applies to the built-in file sink registered in the
        // constructor; sinks added via AddSink() are opaque and are not rotated.
        void SetLogRotationEnabled(bool enabled);
        void SetLogRotationLimits(std::size_t maxFileSizeBytes, std::size_t maxGenerations);

        DebugLogSystem(const DebugLogSystem&) = delete;
        DebugLogSystem& operator=(const DebugLogSystem&) = delete;

    private:
        DebugLogSystem();

        void Dispatch(DebugLogLevel level, std::wstring_view message, std::string_view category);
        bool IsCategoryEnabledLocked(std::string_view category) const;

        struct RegisteredSink { SinkHandle handle; DebugLogLevel minLevel; Sink sink; };
        std::vector<RegisteredSink> m_sinks;
        // Guards m_sinks, m_categoryEnabled and serializes sink invocation; see Dispatch().
        mutable std::mutex m_mutex;
        SinkHandle m_nextHandle = 1;

        std::atomic<bool> m_timestampEnabled{ false };
        std::unordered_map<std::string, bool> m_categoryEnabled;
    };
}
