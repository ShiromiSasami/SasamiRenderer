#include "GerstnerWaveSet.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr float PI = 3.14159265358979f;
        constexpr float DEG_TO_RAD = PI / 180.0f;
    }

    GerstnerWaveSet GerstnerWaveSet::MakeDefault()
    {
        GerstnerWaveSet set;
        set.SetWaves({
            { 14.0f, 0.45f,   0.0f, 0.7f },
            {  8.5f, 0.25f,  18.0f, 0.6f },
            {  5.0f, 0.12f, -24.0f, 0.5f },
            {  2.6f, 0.05f,  40.0f, 0.4f },
        });
        return set;
    }

    // The tangents are the exact partial derivatives of eq.9 with respect to the undisplaced xz,
    // so the normal (dP/dz0 x dP/dx0) and the Jacobian come from the same sums. Q is taken from the
    // unscaled amplitude: shoaling then shrinks both the height and the horizontal sway together.
    GerstnerSample GerstnerWaveSet::Evaluate(float x, float z, float time, float amplitudeScale, float gust) const
    {
        float px = x;
        float py = 0.0f;
        float pz = z;
        float dxx = 1.0f, dxy = 0.0f, dxz = 0.0f;   // dP/dx0
        float dzx = 0.0f, dzy = 0.0f, dzz = 1.0f;   // dP/dz0
        const float count = static_cast<float>((std::max)(m_waves.size(), size_t(1)));

        for (const GerstnerWave& wave : m_waves)
        {
            if (wave.wavelength <= 0.0f || wave.amplitude <= 0.0f)
            {
                continue;
            }
            const float k = 2.0f * PI / wave.wavelength;
            const float omega = std::sqrt(GRAVITY * k);
            const float dirX = std::cos(wave.directionDeg * DEG_TO_RAD);
            const float dirZ = std::sin(wave.directionDeg * DEG_TO_RAD);
            const float q = wave.steepness / (k * wave.amplitude * count);
            const float gustScale = (std::max)(1.0f + wave.gustResponse * (gust - 1.0f), 0.0f);
            const float a = wave.amplitude * amplitudeScale * gustScale;
            const float theta = k * (dirX * x + dirZ * z) - omega * time;
            const float c = std::cos(theta);
            const float s = std::sin(theta);

            px += q * a * dirX * c;
            py += a * s;
            pz += q * a * dirZ * c;

            const float qak = q * a * k;
            const float ak = a * k;
            dxx -= qak * dirX * dirX * s;
            dxy += ak * dirX * c;
            dxz -= qak * dirX * dirZ * s;
            dzx -= qak * dirX * dirZ * s;
            dzy += ak * dirZ * c;
            dzz -= qak * dirZ * dirZ * s;
        }

        GerstnerSample out{};
        out.position[0] = px;
        out.position[1] = py;
        out.position[2] = pz;

        // n = dP/dz0 x dP/dx0 points up for the flat case: (0,0,1) x (1,0,0) = (0,1,0).
        float nx = dzy * dxz - dzz * dxy;
        float ny = dzz * dxx - dzx * dxz;
        float nz = dzx * dxy - dzy * dxx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f)
        {
            nx /= len;
            ny /= len;
            nz /= len;
        }
        else
        {
            nx = 0.0f;
            ny = 1.0f;
            nz = 0.0f;
        }
        out.normal[0] = nx;
        out.normal[1] = ny;
        out.normal[2] = nz;
        out.jacobian = dxx * dzz - dxz * dzx;
        return out;
    }

    uint32_t RunGerstnerSelfTest(std::vector<std::string>* failures, uint32_t* total)
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

        // 1-2: one wave with no sway: crest A at a quarter wavelength, same height one period later.
        {
            GerstnerWaveSet set;
            set.SetWaves({ { 10.0f, 0.5f, 0.0f, 0.0f } });
            const GerstnerSample crest = set.Evaluate(2.5f, 0.0f, 0.0f, 1.0f);
            check(std::fabs(crest.position[1] - 0.5f) < 1e-5f, "crest height != A");
            const float k = 2.0f * PI / 10.0f;
            const float period = 2.0f * PI / std::sqrt(GerstnerWaveSet::GRAVITY * k);
            const GerstnerSample later = set.Evaluate(2.5f, 0.0f, period, 1.0f);
            check(std::fabs(later.position[1] - 0.5f) < 1e-4f, "height not periodic in 2pi/omega");
            check(std::fabs(crest.jacobian - 1.0f) < 1e-6f, "Q=0 jacobian != 1");
        }

        // 4: amplitudeScale 0 is a flat, undisplaced plane.
        {
            const GerstnerWaveSet set = GerstnerWaveSet::MakeDefault();
            const GerstnerSample flat = set.Evaluate(3.0f, -2.0f, 1.7f, 0.0f);
            check(flat.position[0] == 3.0f && flat.position[1] == 0.0f && flat.position[2] == -2.0f &&
                  flat.normal[1] == 1.0f && flat.jacobian == 1.0f, "scale 0 is not flat");
        }

        // 5-6: default set: unit, upward normals; Jacobian equals a central finite difference.
        {
            const GerstnerWaveSet set = GerstnerWaveSet::MakeDefault();
            float worstLen = 0.0f;
            float minNy = 1.0f;
            float worstJ = 0.0f;
            const float h = 1e-3f;
            for (int i = 0; i < 64; ++i)
            {
                const float x = -20.0f + 0.63f * i;
                const float z = 7.0f - 0.41f * i;
                const float t = 0.37f * i;
                const GerstnerSample s = set.Evaluate(x, z, t, 1.0f);
                const float len = std::sqrt(s.normal[0] * s.normal[0] + s.normal[1] * s.normal[1] +
                                            s.normal[2] * s.normal[2]);
                worstLen = (std::max)(worstLen, std::fabs(len - 1.0f));
                minNy = (std::min)(minNy, s.normal[1]);

                const GerstnerSample xp = set.Evaluate(x + h, z, t, 1.0f);
                const GerstnerSample xm = set.Evaluate(x - h, z, t, 1.0f);
                const GerstnerSample zp = set.Evaluate(x, z + h, t, 1.0f);
                const GerstnerSample zm = set.Evaluate(x, z - h, t, 1.0f);
                const float jxx = (xp.position[0] - xm.position[0]) / (2.0f * h);
                const float jxz = (xp.position[2] - xm.position[2]) / (2.0f * h);
                const float jzx = (zp.position[0] - zm.position[0]) / (2.0f * h);
                const float jzz = (zp.position[2] - zm.position[2]) / (2.0f * h);
                worstJ = (std::max)(worstJ, std::fabs((jxx * jzz - jxz * jzx) - s.jacobian));
            }
            check(worstLen < 1e-5f, "normal not unit length");
            check(minNy > 0.0f, "normal points down");
            check(worstJ < 5e-3f, "jacobian disagrees with finite difference");
        }

        // 7-8: one wave folds only past the eq.9 limit: steepness 1 -> min J ~ 0, 1.5 -> min J < 0.
        {
            auto minJacobian = [](float steepness) {
                GerstnerWaveSet set;
                set.SetWaves({ { 10.0f, 0.5f, 30.0f, steepness } });
                float minJ = 1e9f;
                for (int i = 0; i < 400; ++i)
                {
                    minJ = (std::min)(minJ, set.Evaluate(0.05f * i, 0.0f, 0.0f, 1.0f).jacobian);
                }
                return minJ;
            };
            const float at1 = minJacobian(1.0f);
            const float at15 = minJacobian(1.5f);
            check(at1 > -1e-4f && at1 < 0.01f, "steepness 1 min J not ~0");
            check(at15 < -0.4f, "steepness 1.5 does not fold");
        }

        if (total)
        {
            *total = checks;
        }
        return passed;
    }
}
