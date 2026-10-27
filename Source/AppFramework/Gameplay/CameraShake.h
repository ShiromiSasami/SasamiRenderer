#pragma once
#include "Renderer/Animation/AnimMath.h"

#include <cstdint>

// Trauma-based camera shake (Squirrel Eiserloh, "Math for Game Programmers: Juicing Your Cameras
// With Math", GDC 2016): events add trauma in [0, 1], trauma decays linearly, and the shake amount
// is trauma^2 so small hits stay subtle. Offsets come from smooth 1D noise rather than random(),
// which keeps the motion continuous at any frame rate.
namespace SasamiEngine::App::Gameplay
{
    using Render::Anim::Vec3;

    struct CameraShakeSettings
    {
        float maxYaw = 0.08f;       // radians
        float maxPitch = 0.08f;
        float maxRoll = 0.1f;
        float maxOffset = 0.15f;    // world units, applied in camera space
        float frequency = 18.0f;    // noise samples per second
        float decayPerSecond = 0.8f;
        float exponent = 2.0f;      // shake = trauma^exponent
    };

    class CameraShake
    {
    public:
        explicit CameraShake(uint32_t seed = 1u) : m_seed(seed) {}
        void SetSettings(const CameraShakeSettings& settings) { m_settings = settings; }

        void AddTrauma(float amount);   // clamped to [0, 1]
        void Update(float dt);

        float Trauma() const { return m_trauma; }
        float Shake() const;
        float YawOffset() const { return m_yaw; }
        float PitchOffset() const { return m_pitch; }
        float RollOffset() const { return m_roll; }
        Vec3 Offset() const { return m_offset; }    // camera space (x right, y up, z forward)

        // Smooth value noise in [-1, 1] (quintic fade between hashed lattice values).
        static float Noise1D(uint32_t seed, float x);

    private:
        CameraShakeSettings m_settings;
        uint32_t m_seed;
        float m_trauma = 0.0f;
        float m_time = 0.0f;
        float m_yaw = 0.0f;
        float m_pitch = 0.0f;
        float m_roll = 0.0f;
        Vec3 m_offset{};
    };
}

namespace SasamiEngine { using namespace App; }
