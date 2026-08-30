#include "Foundation/Tools/DebugLogSystem.h"
#include "Foundation/Tools/DebugOutput.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <windows.h>

namespace SasamiRenderer
{
    namespace
    {
        constexpr std::string_view kDefaultCategory = "General";

        // --- Timestamp formatting -------------------------------------------------
        // Wall-clock time (not process-uptime) so log lines can be cross-referenced
        // with other logs / external tools.
        std::wstring FormatTimestampPrefix()
        {
            const auto now = std::chrono::system_clock::now();
            const std::time_t nowTimeT = std::chrono::system_clock::to_time_t(now);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

            std::tm localTm{};
            localtime_s(&localTm, &nowTimeT);

            wchar_t buffer[32] = {};
            swprintf_s(buffer, L"[%02d:%02d:%02d.%03lld]", localTm.tm_hour, localTm.tm_min, localTm.tm_sec, static_cast<long long>(ms.count()));
            return buffer;
        }

        // --- Category tagging -------------------------------------------------------
        std::wstring FormatCategoryPrefix(std::string_view category)
        {
            const std::string narrowCategory(category);
            return L"[" + NarrowToWideBestEffort(narrowCategory.c_str()) + L"]";
        }

        // --- File-sink rotation ------------------------------------------------------
        struct LogRotationConfig
        {
            std::atomic<bool> enabled{ true };
            std::atomic<std::size_t> maxSizeBytes{ 10ull * 1024ull * 1024ull };
            std::atomic<std::size_t> maxGenerations{ 3 };
        };

        LogRotationConfig& GetRotationConfig()
        {
            static LogRotationConfig sConfig;
            return sConfig;
        }

        std::filesystem::path MakeGenerationPath(const std::filesystem::path& activePath, std::size_t generation)
        {
            if (generation == 0) {
                return activePath;
            }

            wchar_t genSuffix[16] = {};
            swprintf_s(genSuffix, L"%zu", generation);

            const std::wstring stem = activePath.stem().wstring();
            const std::wstring ext = activePath.extension().wstring();
            return activePath.parent_path() / (stem + L"." + genSuffix + ext);
        }

        // Best-effort: failures are swallowed so a broken filesystem never takes the
        // logging path (and therefore the app) down with it.
        void RotateLogFile(const std::filesystem::path& activePath, std::size_t maxGenerations)
        {
            std::error_code ec;

            if (maxGenerations == 0) {
                std::filesystem::remove(activePath, ec);
                return;
            }

            const auto oldest = MakeGenerationPath(activePath, maxGenerations);
            ec.clear();
            if (std::filesystem::exists(oldest, ec)) {
                ec.clear();
                std::filesystem::remove(oldest, ec);
            }

            for (std::size_t generation = maxGenerations; generation >= 1; --generation) {
                const auto dst = MakeGenerationPath(activePath, generation);
                const auto src = MakeGenerationPath(activePath, generation - 1);
                ec.clear();
                if (std::filesystem::exists(src, ec)) {
                    ec.clear();
                    std::filesystem::rename(src, dst, ec);
                }
            }
        }

        bool ShouldRotate(const std::filesystem::path& activePath, std::size_t pendingBytes, std::size_t maxSizeBytes)
        {
            if (maxSizeBytes == 0) {
                return false;
            }

            std::error_code ec;
            const auto currentSize = std::filesystem::file_size(activePath, ec);
            if (ec) {
                return false;
            }

            return (static_cast<std::uintmax_t>(currentSize) + pendingBytes) > maxSizeBytes;
        }

        struct DebugLogFileState
        {
            std::ofstream stream;
            std::filesystem::path path;
        };

        DebugLogFileState& GetDebugLogFileState()
        {
            static DebugLogFileState sState = []() {
                DebugLogFileState state;
                wchar_t exePath[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                state.path = std::wstring(exePath) + L".log";
                state.stream.open(state.path, std::ios::out | std::ios::trunc);
                return state;
            }();
            return sState;
        }

        // Rotation happens here, guarded by the same lock the caller (Dispatch) already
        // holds around sink invocation -- see the m_mutex comment in the header.
        void WriteToLogFile(std::wstring_view message)
        {
            auto& state = GetDebugLogFileState();
            auto& f = state.stream;
            if (!f.is_open() || message.empty()) {
                return;
            }

            const int utf8Required = ::WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
            if (utf8Required <= 0) {
                return;
            }

            std::string utf8(static_cast<size_t>(utf8Required), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), utf8.data(), utf8Required, nullptr, nullptr);

            auto& rotation = GetRotationConfig();
            if (rotation.enabled.load(std::memory_order_relaxed)) {
                const std::size_t maxBytes = rotation.maxSizeBytes.load(std::memory_order_relaxed);
                const std::size_t maxGenerations = rotation.maxGenerations.load(std::memory_order_relaxed);
                if (ShouldRotate(state.path, utf8.size(), maxBytes)) {
                    f.close();
                    RotateLogFile(state.path, maxGenerations);
                    f.open(state.path, std::ios::out | std::ios::trunc);
                }
            }

            if (!f.is_open()) {
                return;
            }

            f << utf8;
            f.flush();
        }
    }

    DebugLogSystem& DebugLogSystem::Instance()
    {
        static DebugLogSystem instance;
        return instance;
    }

    DebugLogSystem::DebugLogSystem()
    {
        AddSink([](DebugLogLevel, std::wstring_view message) {
            const std::wstring nullTerminated(message);
            ::OutputDebugStringW(nullTerminated.c_str());
        }, DebugLogLevel::Info);

        AddSink([](DebugLogLevel, std::wstring_view message) {
            WriteToLogFile(message);
        }, DebugLogLevel::Info);
    }

    DebugLogSystem::SinkHandle DebugLogSystem::AddSink(Sink sink, DebugLogLevel minLevel)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const SinkHandle handle = m_nextHandle++;
        m_sinks.push_back(RegisteredSink{ handle, minLevel, std::move(sink) });
        return handle;
    }

    void DebugLogSystem::RemoveSink(SinkHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::erase_if(m_sinks, [handle](const RegisteredSink& entry) { return entry.handle == handle; });
    }

    bool DebugLogSystem::IsCategoryEnabledLocked(std::string_view category) const
    {
        const auto it = m_categoryEnabled.find(std::string(category));
        if (it == m_categoryEnabled.end()) {
            // Unregistered categories default to enabled: a missing SetCategoryEnabled()
            // call should never silently drop log output.
            return true;
        }
        return it->second;
    }

    void DebugLogSystem::Dispatch(DebugLogLevel level, std::wstring_view message, std::string_view category)
    {
        // Logging is called from worker threads (asset loading, init tasks) while the
        // main thread logs every frame. The sink list and the sinks themselves -- the
        // file stream in particular -- are not individually thread-safe, and concurrent
        // std::ofstream writes corrupt its internal state rather than merely interleaving
        // output. One lock around dispatch keeps each message atomic and the list stable.
        std::lock_guard<std::mutex> lock(m_mutex);

        const bool explicitCategory = !category.empty();
        const std::string_view filterKey = explicitCategory ? category : kDefaultCategory;
        if (!IsCategoryEnabledLocked(filterKey)) {
            return;
        }

        std::wstring prefix;
        if (m_timestampEnabled.load(std::memory_order_relaxed)) {
            prefix += FormatTimestampPrefix();
        }
        if (explicitCategory) {
            prefix += FormatCategoryPrefix(category);
        }

        std::wstring formatted;
        std::wstring_view outMessage = message;
        if (!prefix.empty()) {
            formatted.reserve(prefix.size() + 1 + message.size());
            formatted += prefix;
            formatted += L' ';
            formatted += message;
            outMessage = formatted;
        }

        for (const auto& entry : m_sinks) {
            if (level >= entry.minLevel) {
                entry.sink(level, outMessage);
            }
        }
    }

    void DebugLogSystem::Log(DebugLogLevel level, const wchar_t* message)
    {
        Log(level, message, std::string_view{});
    }

    void DebugLogSystem::Log(DebugLogLevel level, const char* message)
    {
        Log(level, message, std::string_view{});
    }

    void DebugLogSystem::Log(DebugLogLevel level, const wchar_t* message, std::string_view category)
    {
        if (!message) {
            return;
        }
        Dispatch(level, message, category);
    }

    void DebugLogSystem::Log(DebugLogLevel level, const char* message, std::string_view category)
    {
        if (!message) {
            return;
        }
        const std::wstring wideMessage = NarrowToWideBestEffort(message);
        Dispatch(level, wideMessage, category);
    }

    void DebugLogSystem::SetTimestampEnabled(bool enabled)
    {
        m_timestampEnabled.store(enabled, std::memory_order_relaxed);
    }

    bool DebugLogSystem::IsTimestampEnabled() const
    {
        return m_timestampEnabled.load(std::memory_order_relaxed);
    }

    void DebugLogSystem::SetCategoryEnabled(std::string_view category, bool enabled)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_categoryEnabled[std::string(category)] = enabled;
    }

    bool DebugLogSystem::IsCategoryEnabled(std::string_view category) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return IsCategoryEnabledLocked(category);
    }

    void DebugLogSystem::SetLogRotationEnabled(bool enabled)
    {
        GetRotationConfig().enabled.store(enabled, std::memory_order_relaxed);
    }

    void DebugLogSystem::SetLogRotationLimits(std::size_t maxFileSizeBytes, std::size_t maxGenerations)
    {
        auto& rotation = GetRotationConfig();
        rotation.maxSizeBytes.store(maxFileSizeBytes, std::memory_order_relaxed);
        rotation.maxGenerations.store(maxGenerations, std::memory_order_relaxed);
    }
}
