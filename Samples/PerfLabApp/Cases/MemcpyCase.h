#pragma once

#include "PerfCase.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // Dummy case that exercises the framework: copying `bytes` bytes with a byte-at-a-time loop
    // (baseline) versus std::memcpy (optimised). Check compares both destinations with the source.
    class MemcpyCase : public IPerfCase
    {
    public:
        MemcpyCase(std::string name, uint64_t bytes);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }

        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        std::string m_name;
        std::string m_description;
        uint64_t m_bytes = 0;
        std::vector<uint8_t> m_source;
        std::vector<uint8_t> m_destination[2];
    };
}
