#pragma once

#include <chrono>
#include <cstdio>

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiEngine::Foundation
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

    // Milliseconds from `start` until now, the usual "how long did this take" one-liner.
    inline double MillisecondsSince(ScopedPerfTimer::Timestamp start)
    {
        return ScopedPerfTimer::ElapsedMilliseconds(start, ScopedPerfTimer::Now());
    }
}

// Lets the other layers name Foundation types unqualified. The directive sits on SasamiEngine rather
// than on a layer, so anything nested under it -- SasamiEngine::Render, SasamiEngine::App -- finds these
// names by enclosing-namespace lookup.
//
// The SasamiRenderer -> SasamiEngine::<layer> namespace migration it was written for is finished; what
// is left is deciding whether the layers should qualify Foundation:: explicitly instead. That is not a
// mechanical change (roughly 76 files name these types unqualified), so the directive stays until then.
namespace SasamiEngine { using namespace Foundation; }
