#include "WindFoliage.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr float TWO_PI = 6.28318530718f;
        constexpr float MAX_LEAN = 0.12f;       // fraction of the blade height
        constexpr float NORMAL_TILT = 0.6f;     // blend toward +Y: WaveApp has no ambient, so the side facing away from the sun drew black at 0.3

        // PCG32 (O'Neill 2014), output mapped to [0,1).
        class Pcg32
        {
        public:
            explicit Pcg32(uint32_t seed)
            {
                m_state = 0u;
                Next();
                m_state += seed;
                Next();
            }

            uint32_t Next()
            {
                const uint64_t old = m_state;
                m_state = old * 6364136223846793005ull + 1442695040888963407ull;
                const uint32_t xorShifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
                const uint32_t rot = static_cast<uint32_t>(old >> 59u);
                return (xorShifted >> rot) | (xorShifted << ((32u - rot) & 31u));
            }

            float NextFloat()
            {
                return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f);
            }

        private:
            uint64_t m_state = 0;
        };

        float SelfTestGround(float x, float /*z*/)
        {
            return x > 0.0f ? 0.5f : -1.0f;
        }

        void Normalize3(float v[3])
        {
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 1e-8f)
            {
                const float inv = 1.0f / len;
                v[0] *= inv;
                v[1] *= inv;
                v[2] *= inv;
            }
        }

        float Distance3(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0];
            const float dy = a[1] - b[1];
            const float dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    namespace
    {
        // Rewrites the 16 vertices of one blade. dir is the unit xz bend direction, bend the
        // dimensionless factor (tip offset = bend * height before the length is restored), flutter
        // the tip sway in metres across dir.
        void WriteBlade(Vertex* verts, const float root[3], float height, const float facing[2],
                        const float lean[2], const float dir[2], float bend, float flutter,
                        uint32_t rows, const WindFoliageSettings& s)
        {
            const float perp[2] = { -dir[1], dir[0] };
            float q[8][3];
            for (uint32_t r = 0; r < rows; ++r)
            {
                const float t = static_cast<float>(r) / static_cast<float>(rows - 1);
                const float t2 = t * t;
                const float rest[3] = { lean[0] * t2, height * t, lean[1] * t2 };
                const float restLen = std::sqrt(rest[0] * rest[0] + rest[1] * rest[1] + rest[2] * rest[2]);
                float off[3] = {
                    rest[0] + dir[0] * bend * height * t2 + perp[0] * flutter * t2,
                    rest[1],
                    rest[2] + dir[1] * bend * height * t2 + perp[1] * flutter * t2
                };
                const float len = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
                if (len > 1e-8f)
                {
                    const float k = restLen / len;
                    off[0] *= k;
                    off[1] *= k;
                    off[2] *= k;
                }
                q[r][0] = root[0] + off[0];
                q[r][1] = root[1] + off[1];
                q[r][2] = root[2] + off[2];
            }

            for (uint32_t r = 0; r < rows; ++r)
            {
                const float t = static_cast<float>(r) / static_cast<float>(rows - 1);
                const float hw = 0.5f * s.width * (1.0f - t);

                const uint32_t a = (r + 1 < rows) ? r : r - 1;
                const float tan[3] = { q[a + 1][0] - q[a][0], q[a + 1][1] - q[a][1], q[a + 1][2] - q[a][2] };
                const float axis[3] = { facing[0], 0.0f, facing[1] };
                float n[3] = {
                    tan[1] * axis[2] - tan[2] * axis[1],
                    tan[2] * axis[0] - tan[0] * axis[2],
                    tan[0] * axis[1] - tan[1] * axis[0]
                };
                Normalize3(n);
                float nA[3];
                float nB[3];
                for (int c = 0; c < 3; ++c)
                {
                    const float up = (c == 1) ? 1.0f : 0.0f;
                    nA[c] = n[c] + (up - n[c]) * NORMAL_TILT;
                    nB[c] = -n[c] + (up + n[c]) * NORMAL_TILT;
                }
                Normalize3(nA);
                Normalize3(nB);

                for (uint32_t side = 0; side < 2; ++side)
                {
                    const float* normal = side == 0 ? nA : nB;
                    for (uint32_t lr = 0; lr < 2; ++lr)
                    {
                        const float sign = lr == 0 ? -1.0f : 1.0f;
                        Vertex& v = verts[side * rows * 2 + r * 2 + lr];
                        v.position[0] = q[r][0] + axis[0] * hw * sign;
                        v.position[1] = q[r][1];
                        v.position[2] = q[r][2] + axis[2] * hw * sign;
                        v.normal[0] = normal[0];
                        v.normal[1] = normal[1];
                        v.normal[2] = normal[2];
                        for (int c = 0; c < 3; ++c)
                        {
                            v.color[c] = s.rootColor[c] + (s.tipColor[c] - s.rootColor[c]) * t;
                        }
                        v.color[3] = 1.0f;
                        v.uv[0] = static_cast<float>(lr);
                        v.uv[1] = t;
                    }
                }
            }
        }
    }

    void WindFoliage::Configure(const WindFoliageSettings& settings, SeaBedFunction groundHeight)
    {
        m_settings = settings;
        m_blades.clear();
        m_blades.reserve(m_settings.bladeCount);

        Pcg32 rng(m_settings.seed);
        const uint64_t maxAttempts = static_cast<uint64_t>(m_settings.bladeCount) * 8u;
        for (uint64_t attempt = 0; attempt < maxAttempts && m_blades.size() < m_settings.bladeCount; ++attempt)
        {
            const float x = m_settings.areaMin[0] + (m_settings.areaMax[0] - m_settings.areaMin[0]) * rng.NextFloat();
            const float z = m_settings.areaMin[1] + (m_settings.areaMax[1] - m_settings.areaMin[1]) * rng.NextFloat();
            const float ground = groundHeight ? groundHeight(x, z) : 0.0f;
            if (groundHeight && !(ground > m_settings.minGroundHeight))
            {
                continue;
            }

            Blade blade{};
            blade.root[0] = x;
            blade.root[1] = ground - 0.02f;
            blade.root[2] = z;
            blade.height = m_settings.heightMin + (m_settings.heightMax - m_settings.heightMin) * rng.NextFloat();
            const float facingAngle = TWO_PI * rng.NextFloat();
            blade.facing[0] = std::cos(facingAngle);
            blade.facing[1] = std::sin(facingAngle);
            blade.phase = TWO_PI * rng.NextFloat();
            const float leanAngle = TWO_PI * rng.NextFloat();
            const float leanLength = MAX_LEAN * blade.height * rng.NextFloat();
            blade.lean[0] = std::cos(leanAngle) * leanLength;
            blade.lean[1] = std::sin(leanAngle) * leanLength;
            m_blades.push_back(blade);
        }

        const uint32_t bladeCount = static_cast<uint32_t>(m_blades.size());
        m_mesh.vertices.assign(static_cast<size_t>(bladeCount) * VERTS_PER_BLADE, Vertex{});
        m_mesh.indices.clear();
        m_mesh.indices.reserve(static_cast<size_t>(bladeCount) * (ROWS - 1) * 12);
        for (uint32_t b = 0; b < bladeCount; ++b)
        {
            const uint32_t base = b * VERTS_PER_BLADE;
            for (uint32_t side = 0; side < 2; ++side)
            {
                const uint32_t sideBase = base + side * VERTS_PER_SIDE;
                for (uint32_t r = 0; r + 1 < ROWS; ++r)
                {
                    const uint32_t l0 = sideBase + r * 2;
                    const uint32_t r0 = l0 + 1;
                    const uint32_t l1 = l0 + 2;
                    const uint32_t r1 = l0 + 3;
                    if (side == 0)
                    {
                        m_mesh.indices.insert(m_mesh.indices.end(), { l0, l1, r0, r0, l1, r1 });
                    }
                    else
                    {
                        m_mesh.indices.insert(m_mesh.indices.end(), { l0, r0, l1, r0, r1, l1 });
                    }
                }
            }
        }

        const float restDir[2] = { 1.0f, 0.0f };
        for (uint32_t b = 0; b < bladeCount; ++b)
        {
            const Blade& blade = m_blades[b];
            WriteBlade(&m_mesh.vertices[static_cast<size_t>(b) * VERTS_PER_BLADE], blade.root, blade.height,
                       blade.facing, blade.lean, restDir, 0.0f, 0.0f, ROWS, m_settings);
        }

        m_stats = {};
        m_stats.blades = bladeCount;
        m_stats.vertices = bladeCount * VERTS_PER_BLADE;
    }

    void WindFoliage::Update(const CoastalWindField& wind, float time)
    {
        const uint32_t bladeCount = static_cast<uint32_t>(m_blades.size());
        if (m_mesh.vertices.size() != static_cast<size_t>(bladeCount) * VERTS_PER_BLADE)
        {
            return;
        }

        double bendSum = 0.0;
        double gustSum = 0.0;
        float bendMax = 0.0f;
        for (uint32_t b = 0; b < bladeCount; ++b)
        {
            const Blade& blade = m_blades[b];
            const CoastalWindSample w = wind.Sample(blade.root[0], blade.root[2], time);

            float dir[2] = { 1.0f, 0.0f };
            float bend = 0.0f;
            if (w.speed >= 1e-4f)
            {
                const float inv = 1.0f / (std::max)(w.speed, 1e-4f);
                dir[0] = w.velocity[0] * inv;
                dir[1] = w.velocity[1] * inv;
                bend = std::clamp(m_settings.stiffness * w.speed * w.speed, 0.0f, m_settings.maxBend);
            }
            const float flutter = m_settings.flutter * w.gust
                * std::sin(TWO_PI * m_settings.flutterFrequency * time + blade.phase);

            WriteBlade(&m_mesh.vertices[static_cast<size_t>(b) * VERTS_PER_BLADE], blade.root, blade.height,
                       blade.facing, blade.lean, dir, bend, flutter, ROWS, m_settings);

            bendSum += bend;
            gustSum += w.gust;
            bendMax = (std::max)(bendMax, bend);
        }

        const double n = static_cast<double>((std::max)(bladeCount, 1u));
        m_stats.blades = bladeCount;
        m_stats.vertices = bladeCount * VERTS_PER_BLADE;
        m_stats.meanBend = static_cast<float>(bendSum / n);
        m_stats.maxBend = bendMax;
        m_stats.meanGust = static_cast<float>(gustSum / n);
    }

    uint32_t RunWindFoliageSelfTest(std::vector<std::string>* failures, uint32_t* total)
    {
        uint32_t passed = 0;
        uint32_t count = 0;
        const auto check = [&](bool ok, const char* name)
        {
            ++count;
            if (ok)
            {
                ++passed;
            }
            else if (failures)
            {
                failures->push_back(name);
            }
        };

        WindFoliageSettings settings;
        settings.bladeCount = 200;
        settings.areaMin[0] = -10.0f;
        settings.areaMin[1] = -5.0f;
        settings.areaMax[0] = 10.0f;
        settings.areaMax[1] = 5.0f;
        settings.flutter = 0.0f;

        WindFoliage foliage;
        foliage.Configure(settings, &SelfTestGround);
        const uint32_t bladeCount = foliage.Stats().blades;
        const uint32_t vps = WindFoliage::VERTS_PER_BLADE;
        const uint32_t rows = WindFoliage::ROWS;

        const auto rowMid = [&](uint32_t blade, uint32_t row, float out[3])
        {
            const std::vector<Vertex>& v = foliage.GetMesh().vertices;
            const Vertex& a = v[blade * vps + row * 2];
            const Vertex& b = v[blade * vps + row * 2 + 1];
            for (int c = 0; c < 3; ++c)
            {
                out[c] = 0.5f * (a.position[c] + b.position[c]);
            }
        };

        check(bladeCount > 0, "foliage: no blades placed");

        bool rootsOk = true;
        for (uint32_t b = 0; b < bladeCount; ++b)
        {
            float root[3];
            rowMid(b, 0, root);
            rootsOk = rootsOk && root[0] > -0.1f;
        }
        check(rootsOk, "foliage: blade rooted off the dry ground");

        CoastalWindField wind;
        CoastalWindSettings ws;
        ws.speed = 0.0f;
        ws.gustStrength = 0.0f;
        ws.gustVeerDeg = 0.0f;
        ws.directionDeg = 0.0f;
        wind.SetSettings(ws);

        foliage.Update(wind, 0.0f);
        std::vector<float> restLen(static_cast<size_t>(bladeCount) * rows, 0.0f);
        std::vector<float> restTipY(bladeCount, 0.0f);
        bool restOk = true;
        for (uint32_t b = 0; b < bladeCount; ++b)
        {
            float root[3];
            rowMid(b, 0, root);
            for (uint32_t r = 0; r < rows; ++r)
            {
                float p[3];
                rowMid(b, r, p);
                restLen[b * rows + r] = Distance3(p, root);
            }
            float tip[3];
            rowMid(b, rows - 1, tip);
            restTipY[b] = tip[1];
            const float tipLen = restLen[b * rows + rows - 1];
            restOk = restOk && tip[1] > root[1] + 0.9f * settings.heightMin
                && tipLen >= settings.heightMin - 1e-4f
                && tipLen <= settings.heightMax * 1.2f;
        }
        check(restOk, "foliage: no-wind tip is not at rest height");

        ws.speed = 12.0f;
        wind.SetSettings(ws);
        foliage.Update(wind, 0.0f);
        bool bendOk = true;
        bool lengthOk = true;
        for (uint32_t b = 0; b < bladeCount; ++b)
        {
            float root[3];
            float tip[3];
            rowMid(b, 0, root);
            rowMid(b, rows - 1, tip);
            bendOk = bendOk && (tip[0] - root[0]) > MAX_LEAN * settings.heightMax && tip[1] < restTipY[b];
            for (uint32_t r = 0; r < rows; ++r)
            {
                float p[3];
                rowMid(b, r, p);
                lengthOk = lengthOk && std::fabs(Distance3(p, root) - restLen[b * rows + r]) < 1e-4f;
            }
        }
        check(bendOk, "foliage: +x wind does not bend and lower the tip");
        check(lengthOk, "foliage: blade length not preserved under bend");

        const auto meanDrop = [&](float speed)
        {
            CoastalWindSettings s = ws;
            s.speed = speed;
            wind.SetSettings(s);
            foliage.Update(wind, 0.0f);
            double drop = 0.0;
            for (uint32_t b = 0; b < bladeCount; ++b)
            {
                float tip[3];
                rowMid(b, rows - 1, tip);
                drop += restTipY[b] - tip[1];
            }
            return drop / static_cast<double>((std::max)(bladeCount, 1u));
        };
        check(meanDrop(20.0f) > meanDrop(8.0f), "foliage: stronger wind does not bend more");

        if (total)
        {
            *total = count;
        }
        return passed;
    }
}
