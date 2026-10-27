#include "CoastalWindField.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr float PI = 3.14159265358979f;
        constexpr float DEG_TO_RAD = PI / 180.0f;

        // PCG-style integer hash of a lattice point, mapped to [0,1).
        float HashLattice(int32_t ix, int32_t iz, uint32_t seed)
        {
            uint32_t h = static_cast<uint32_t>(ix) * 0x8da6b343u ^ static_cast<uint32_t>(iz) * 0xd8163841u ^ seed * 0xcb1ab31fu;
            h = h * 747796405u + 2891336453u;
            h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
            h = (h >> 22u) ^ h;
            return static_cast<float>(h >> 8u) * (1.0f / 16777216.0f);
        }

        float Quintic(float t)
        {
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        // Bilinear lattice value noise with quintic weights, result in [0,1].
        float ValueNoise2D(float u, float v)
        {
            const float fu = std::floor(u);
            const float fv = std::floor(v);
            const int32_t iu = static_cast<int32_t>(fu);
            const int32_t iv = static_cast<int32_t>(fv);
            const float tu = Quintic(u - fu);
            const float tv = Quintic(v - fv);
            const float h00 = HashLattice(iu, iv, 0u);
            const float h10 = HashLattice(iu + 1, iv, 0u);
            const float h01 = HashLattice(iu, iv + 1, 0u);
            const float h11 = HashLattice(iu + 1, iv + 1, 0u);
            const float a = h00 + (h10 - h00) * tu;
            const float b = h01 + (h11 - h01) * tu;
            return a + (b - a) * tv;
        }
    }

    void CoastalWindField::MeanDirection(float outDir[2]) const
    {
        const float angle = m_settings.directionDeg * DEG_TO_RAD;
        outDir[0] = std::cos(angle);
        outDir[1] = std::sin(angle);
    }

    CoastalWindSample CoastalWindField::Sample(float x, float z, float time) const
    {
        float d[2];
        MeanDirection(d);
        const float perp[2] = { -d[1], d[0] };
        const float speed = (std::max)(m_settings.speed, 0.0f);

        // Frozen turbulence: the pattern moves with the mean wind.
        const float px = x - d[0] * speed * time;
        const float pz = z - d[1] * speed * time;
        const float along = px * d[0] + pz * d[1];
        const float across = px * perp[0] + pz * perp[1];
        const float u = along / (std::max)(m_settings.gustLength, 0.1f);
        const float v = across / (std::max)(m_settings.gustWidth, 0.1f);

        const float n = 0.65f * ValueNoise2D(u, v) + 0.35f * ValueNoise2D(2.03f * u + 17.1f, 2.03f * v - 5.3f);
        const float strength = (std::min)((std::max)(m_settings.gustStrength, 0.0f), 1.0f);
        const float gust = (std::max)(0.0f, 1.0f + strength * (2.0f * n - 1.0f));

        const float w = ValueNoise2D(u * 0.7f + 41.7f, v * 0.7f + 9.2f);
        const float angle = (2.0f * w - 1.0f) * m_settings.gustVeerDeg * DEG_TO_RAD;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float dl[2] = { d[0] * c - d[1] * s, d[0] * s + d[1] * c };

        CoastalWindSample out{};
        out.speed = speed * gust;
        out.velocity[0] = dl[0] * out.speed;
        out.velocity[1] = dl[1] * out.speed;
        out.gust = gust;
        return out;
    }

    uint32_t RunCoastalWindSelfTest(std::vector<std::string>* failures, uint32_t* total)
    {
        uint32_t passed = 0;
        uint32_t checks = 0;
        auto check = [&](bool ok, const std::string& what) {
            ++checks;
            if (ok)
            {
                ++passed;
            }
            else if (failures)
            {
                failures->push_back(what);
            }
        };

        // 1: no gust and no veer gives the uniform mean wind.
        {
            CoastalWindField field;
            CoastalWindSettings s;
            s.directionDeg = 33.0f;
            s.gustStrength = 0.0f;
            s.gustVeerDeg = 0.0f;
            field.SetSettings(s);
            float d[2];
            field.MeanDirection(d);
            float worst = 0.0f;
            for (int i = 0; i < 32; ++i)
            {
                const CoastalWindSample r = field.Sample(-40.0f + 3.7f * i, 25.0f - 1.9f * i, 0.43f * i);
                worst = (std::max)(worst, std::fabs(r.velocity[0] - s.speed * d[0]));
                worst = (std::max)(worst, std::fabs(r.velocity[1] - s.speed * d[1]));
                worst = (std::max)(worst, std::fabs(r.gust - 1.0f));
            }
            check(worst < 1e-5f, "zero gust strength is not the uniform mean wind");
        }

        // 2: Taylor property, Sample(p + U d t, t) == Sample(p, 0).
        {
            CoastalWindField field;
            field.MutableSettings().directionDeg = 25.0f;
            const CoastalWindSettings& s = field.Settings();
            float d[2];
            field.MeanDirection(d);
            float worst = 0.0f;
            for (int i = 0; i < 32; ++i)
            {
                const float x = -30.0f + 2.9f * i;
                const float z = 12.0f - 1.7f * i;
                const float t = 0.31f * i;
                const CoastalWindSample a = field.Sample(x + s.speed * d[0] * t, z + s.speed * d[1] * t, t);
                const CoastalWindSample b = field.Sample(x, z, 0.0f);
                worst = (std::max)(worst, std::fabs(a.velocity[0] - b.velocity[0]));
                worst = (std::max)(worst, std::fabs(a.velocity[1] - b.velocity[1]));
                worst = (std::max)(worst, std::fabs(a.gust - b.gust));
            }
            check(worst < 1e-3f, "gust pattern is not advected with the mean wind");
        }

        // 3-4: mean gust near 1; speed and gust never negative even at full strength.
        {
            CoastalWindField field;
            double sum = 0.0;
            for (int j = 0; j < 64; ++j)
            {
                for (int i = 0; i < 64; ++i)
                {
                    sum += field.Sample(1.5f * i, 1.5f * j, 0.0f).gust;
                }
            }
            const float mean = static_cast<float>(sum / (64.0 * 64.0));
            check(mean >= 0.85f && mean <= 1.15f, "mean gust is not ~1");

            field.MutableSettings().gustStrength = 1.0f;
            bool nonNegative = true;
            for (int j = 0; j < 64; ++j)
            {
                for (int i = 0; i < 64; ++i)
                {
                    const CoastalWindSample r = field.Sample(1.5f * i, 1.5f * j, 0.0f);
                    nonNegative = nonNegative && r.speed >= 0.0f && r.gust >= 0.0f;
                }
            }
            check(nonNegative, "negative speed or gust");
        }

        // 5: direction 90 degrees blows toward +z.
        {
            CoastalWindField field;
            CoastalWindSettings s;
            s.directionDeg = 90.0f;
            s.gustStrength = 0.0f;
            s.gustVeerDeg = 0.0f;
            field.SetSettings(s);
            const CoastalWindSample r = field.Sample(5.0f, -3.0f, 1.0f);
            check(std::fabs(r.velocity[0]) < 1e-4f && std::fabs(r.velocity[1] - s.speed) < 1e-4f,
                  "direction 90 is not +z");
        }

        if (total)
        {
            *total = checks;
        }
        return passed;
    }
}
