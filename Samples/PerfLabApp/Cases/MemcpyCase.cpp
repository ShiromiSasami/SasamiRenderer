#include "Cases/MemcpyCase.h"

#include "Foundation/Tools/ScopedPerfTimer.h"

#include <cstring>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        // Kept out of line and unvectorised so the optimiser cannot turn it into memcpy itself.
        __declspec(noinline) void CopyBytes(uint8_t* dst, const uint8_t* src, size_t count)
        {
#pragma loop(no_vector)
            for (size_t i = 0; i < count; ++i) {
                dst[i] = src[i];
            }
        }
    }

    MemcpyCase::MemcpyCase(std::string name, uint64_t bytes)
        : m_name(std::move(name))
        , m_bytes(bytes)
    {
        m_description = "copy " + std::to_string(bytes >> 10) + " KiB: byte loop vs std::memcpy (framework check)";
    }

    bool MemcpyCase::Prepare(std::string& error)
    {
        try {
            m_source.resize(static_cast<size_t>(m_bytes));
            uint32_t state = 0x9E3779B9u;
            for (uint8_t& b : m_source) {
                state = state * 1664525u + 1013904223u;
                b = static_cast<uint8_t>(state >> 24);
            }
            // Allocated and touched here, so a run measures the copy, not page faults.
            for (auto& dst : m_destination) {
                dst.assign(static_cast<size_t>(m_bytes), 0);
            }
        } catch (const std::bad_alloc&) {
            error = "out of memory for " + std::to_string(m_bytes) + " bytes";
            Release();
            return false;
        }
        return true;
    }

    PerfMetrics MemcpyCase::RunVariant(PerfVariant variant)
    {
        std::vector<uint8_t>& dst = m_destination[static_cast<uint32_t>(variant)];
        std::memset(dst.data(), 0, dst.size());

        PerfMetrics metrics;
        const auto start = Foundation::ScopedPerfTimer::Now();
        if (variant == PerfVariant::Baseline) {
            CopyBytes(dst.data(), m_source.data(), dst.size());
        } else {
            std::memcpy(dst.data(), m_source.data(), dst.size());
        }
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        metrics.cpuBytes = 2 * m_bytes;   // source + destination
        return metrics;
    }

    bool MemcpyCase::Check(std::string& detail)
    {
        for (uint32_t v = 0; v < 2; ++v) {
            if (m_destination[v].size() != m_source.size()
                || std::memcmp(m_destination[v].data(), m_source.data(), m_source.size()) != 0) {
                detail = std::string(PerfVariantName(static_cast<PerfVariant>(v))) + " differs from the source";
                return false;
            }
        }
        detail = "bit-identical (" + std::to_string(m_bytes) + " bytes)";
        return true;
    }

    void MemcpyCase::Release()
    {
        m_source.clear();
        m_source.shrink_to_fit();
        for (auto& dst : m_destination) {
            dst.clear();
            dst.shrink_to_fit();
        }
    }
}
