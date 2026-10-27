#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct GerstnerWave
    {
        float wavelength = 8.0f;   // m
        float amplitude = 0.3f;    // m
        float directionDeg = 0.0f; // travel direction in the xz plane, 0 = +x
        float steepness = 0.6f;    // 0..1, scaled to Q = steepness / (k A N) (Finch 2004 eq.9 limit)
        float gustResponse = 0.0f; // 0..1, amplitude *= 1 + gustResponse * (gust - 1) (wind ripples)
    };

    struct GerstnerSample
    {
        float position[3];  // displaced surface point
        float normal[3];    // unit normal
        float jacobian;     // det of the horizontal displacement map; < 0 means the crest folded over
    };

    // Sum of Gerstner waves (GPU Gems 1 ch.1, Finch 2004, eq.9 position / eq.12 normal), deep-water
    // dispersion w = sqrt(g k). The horizontal Jacobian (Tessendorf 2001 sec.4.6) marks where crests
    // pinch: J = Jxx Jzz - Jxz^2 with Jxx = 1 - sum Q A k Dx^2 sin, Jzz = 1 - sum Q A k Dz^2 sin,
    // Jxz = -sum Q A k Dx Dz sin. `amplitudeScale` scales every amplitude (shoaling near the shore).
    class GerstnerWaveSet
    {
    public:
        static constexpr float GRAVITY = 9.81f;

        void SetWaves(std::vector<GerstnerWave> waves) { m_waves = std::move(waves); }
        const std::vector<GerstnerWave>& Waves() const { return m_waves; }
        std::vector<GerstnerWave>& MutableWaves() { return m_waves; }

        // Four waves of a moderate swell heading roughly +x (toward the beach).
        static GerstnerWaveSet MakeDefault();

        // `gust` is the local wind gust factor (1 = mean wind); only waves with gustResponse > 0 use it.
        GerstnerSample Evaluate(float x, float z, float time, float amplitudeScale, float gust = 1.0f) const;

    private:
        std::vector<GerstnerWave> m_waves;
    };

    // Analytic checks: a single wave's crest height/period, normal unit length, and that the Jacobian
    // falls below 0 only when Q k A exceeds 1. Returns the number of checks that passed; failures
    // append a description to `failures`.
    uint32_t RunGerstnerSelfTest(std::vector<std::string>* failures, uint32_t* total);
}
