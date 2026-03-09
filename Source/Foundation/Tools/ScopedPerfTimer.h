#pragma once

#include <chrono>
#include <cstdio>

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    class ScopedPerfTimer
    {
    public:
        using Timestamp = std::chrono::steady_clock::time_point;

        static Timestamp Now()
        {
            return std::chrono::steady_clock::now();
        }

        static double ElapsedMilliseconds(Timestamp startTime, Timestamp endTime)
        {
            if (endTime < startTime) {
                return 0.0;
            }

            return std::chrono::duration<double, std::milli>(endTime - startTime).count();
        }

        static void LogMilliseconds(const char* label, double milliseconds)
        {
            if (!label) {
                return;
            }

            char buffer[512] = {};
            const int len = ::snprintf(buffer,
                                       sizeof(buffer),
                                       "[Perf] %s: %.3f ms\n",
                                       label,
                                       milliseconds);
            if (len > 0) {
                DebugLog(buffer);
            }
        }

        explicit ScopedPerfTimer(const char* label)
            : m_label(label)
            , m_startTime(Now())
        {
        }

        ~ScopedPerfTimer()
        {
            StopAndLog();
        }

        void StopAndLog()
        {
            if (m_stopped) {
                return;
            }
            m_stopped = true;

            const Timestamp endTime = Now();
            LogMilliseconds(m_label, ElapsedMilliseconds(m_startTime, endTime));
        }

    private:
        const char* m_label = nullptr;
        Timestamp m_startTime{};
        bool m_stopped = false;
    };
}
