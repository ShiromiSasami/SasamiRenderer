#include "Gameplay/CameraShake.h"

#include <algorithm>
#include <cmath>

// Reference: Squirrel Eiserloh, "Math for Game Programmers: Juicing Your Cameras With Math", GDC 2016
// (trauma, shake = trauma^2, smooth noise instead of random()).
namespace SasamiEngine::App::Gameplay
{
    namespace
    {
        // PCG output hash (O'Neill), integer only so results match across compilers.
        uint32_t PcgHash(uint32_t input)
        {
            const uint32_t state = input * 747796405u + 2891336453u;
            const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
            return (word >> 22u) ^ word;
        }

        float LatticeValue(uint32_t seed, int32_t i)
        {
            const uint32_t h = PcgHash((seed * 0x9E3779B9u) ^ static_cast<uint32_t>(i));
            return static_cast<float>(h >> 8u) * (2.0f / 16777216.0f) - 1.0f;
        }
    }

    void CameraShake::AddTrauma(float amount)
    {
        m_trauma = std::clamp(m_trauma + amount, 0.0f, 1.0f);
    }

    float CameraShake::Shake() const
    {
        return std::pow(m_trauma, m_settings.exponent);
    }

    void CameraShake::Update(float dt)
    {
        m_time += dt;
        m_trauma = std::max(0.0f, m_trauma - m_settings.decayPerSecond * dt);

        const float s = Shake();
        const float x = m_time * m_settings.frequency;
        m_yaw = m_settings.maxYaw * s * Noise1D(m_seed, x);
        m_pitch = m_settings.maxPitch * s * Noise1D(m_seed + 1u, x);
        m_roll = m_settings.maxRoll * s * Noise1D(m_seed + 2u, x);
        m_offset = {
            m_settings.maxOffset * s * Noise1D(m_seed + 3u, x),
            m_settings.maxOffset * s * Noise1D(m_seed + 4u, x),
            m_settings.maxOffset * s * Noise1D(m_seed + 5u, x),
        };
    }

    float CameraShake::Noise1D(uint32_t seed, float x)
    {
        const float fl = std::floor(x);
        const int32_t i = static_cast<int32_t>(fl);
        const float t = x - fl;
        const float f = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        const float a = LatticeValue(seed, i);
        const float b = LatticeValue(seed, i + 1);
        return a + (b - a) * f;
    }
}
