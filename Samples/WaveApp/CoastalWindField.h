#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct CoastalWindSettings
    {
        float speed = 10.0f;          // m/s, mean wind U10 (10 m above the sea)
        float directionDeg = 0.0f;    // direction the wind blows toward in the xz plane, 0 = +x (onshore)
        float gustStrength = 0.6f;    // 0..1, local speed = speed * (1 + gustStrength * (2 n - 1)), n in [0,1]
        float gustLength = 14.0f;     // m, gust cell size along the wind
        float gustWidth = 7.0f;       // m, gust cell size across the wind (gusts are streaks along the flow)
        float gustVeerDeg = 12.0f;    // max local direction wobble
    };

    struct CoastalWindSample
    {
        float velocity[2];   // m/s in xz
        float speed;         // |velocity|
        float gust;          // speed / mean speed, >= 0; 1 = calm patch at the mean wind
    };

    // One analytic wind over the coast that every wind-driven effect samples, so they stay in sync.
    // Gusts are 2-octave value noise in a frame aligned with the mean wind, advected at the mean wind
    // speed: g(x, t) = n(x - U t). That is Taylor's frozen-turbulence hypothesis (Taylor 1938): the
    // gust pattern is carried by the mean flow without changing shape, so a gust that flattens the
    // grass at one point is the same one that roughens the water upwind of it a moment earlier.
    // Height-independent (2D) on purpose; there is no obstacle or terrain interaction.
    class CoastalWindField
    {
    public:
        void SetSettings(const CoastalWindSettings& settings) { m_settings = settings; }
        const CoastalWindSettings& Settings() const { return m_settings; }
        CoastalWindSettings& MutableSettings() { return m_settings; }

        // Unit vector of the mean direction.
        void MeanDirection(float outDir[2]) const;

        CoastalWindSample Sample(float x, float z, float time) const;

    private:
        CoastalWindSettings m_settings;
    };

    // Checks: zero gust strength gives the uniform mean wind; the Taylor property
    // Sample(p + U d t, t) == Sample(p, 0); gust mean ~1 over many points; speed never negative.
    uint32_t RunCoastalWindSelfTest(std::vector<std::string>* failures, uint32_t* total);
}
