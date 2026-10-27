#pragma once

#include <cstdint>

namespace SasamiEngine::App
{
    // Seedable random stream shared by the placement generators, so a given seed reproduces the
    // same layout on every machine (std::uniform_real_distribution is implementation-defined).
    // PCG-RXS-M-XS 32-bit hash from Jarzynski & Olano, "Hash Functions for GPU Rendering",
    // JCGT 9(3), 2020; the stream is the hash chained on its own output.
    class PlacementRandom
    {
    public:
        explicit PlacementRandom(uint32_t seed) : m_state(Hash(seed)) {}

        // [0, 1) with 24 bits of mantissa, so 1.0 is never returned.
        float NextUnit()
        {
            m_state = Hash(m_state);
            return static_cast<float>(m_state >> 8) * (1.0f / 16777216.0f);
        }

    private:
        static uint32_t Hash(uint32_t input)
        {
            const uint32_t state = input * 747796405u + 2891336453u;
            const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
            return (word >> 22u) ^ word;
        }

        uint32_t m_state;
    };
}
