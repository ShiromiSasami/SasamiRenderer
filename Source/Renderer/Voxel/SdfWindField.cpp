#include "Renderer/Voxel/SdfWindField.h"

#include "Renderer/Voxel/SdfSphereTracer.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        // murmur3 finalizer over the combined lattice coordinates and channel.
        uint32_t HashLattice(int32_t ix, int32_t iy, int32_t iz, uint32_t channel)
        {
            uint32_t h = channel * 0x9e3779b9u;
            h ^= static_cast<uint32_t>(ix) * 0x85ebca6bu;
            h = (h << 13) | (h >> 19);
            h ^= static_cast<uint32_t>(iy) * 0xc2b2ae35u;
            h = (h << 13) | (h >> 19);
            h ^= static_cast<uint32_t>(iz) * 0x27d4eb2fu;
            h ^= h >> 16;
            h *= 0x85ebca6bu;
            h ^= h >> 13;
            h *= 0xc2b2ae35u;
            h ^= h >> 16;
            return h;
        }

        // Perlin's 12 edge-centre gradients.
        float GradientDot(uint32_t hash, float x, float y, float z)
        {
            switch (hash % 12u) {
            case 0: return x + y;
            case 1: return -x + y;
            case 2: return x - y;
            case 3: return -x - y;
            case 4: return x + z;
            case 5: return -x + z;
            case 6: return x - z;
            case 7: return -x - z;
            case 8: return y + z;
            case 9: return -y + z;
            case 10: return y - z;
            default: return -y - z;
            }
        }

        float Fade(float t)
        {
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        float GradientNoise(float x, float y, float z, uint32_t channel)
        {
            const float fx = std::floor(x);
            const float fy = std::floor(y);
            const float fz = std::floor(z);
            const int32_t ix = static_cast<int32_t>(fx);
            const int32_t iy = static_cast<int32_t>(fy);
            const int32_t iz = static_cast<int32_t>(fz);
            const float tx = x - fx;
            const float ty = y - fy;
            const float tz = z - fz;
            const float u = Fade(tx);
            const float v = Fade(ty);
            const float w = Fade(tz);

            float corner[8];
            for (int i = 0; i < 8; ++i) {
                const int dx = i & 1;
                const int dy = (i >> 1) & 1;
                const int dz = (i >> 2) & 1;
                corner[i] = GradientDot(HashLattice(ix + dx, iy + dy, iz + dz, channel),
                                        tx - static_cast<float>(dx), ty - static_cast<float>(dy),
                                        tz - static_cast<float>(dz));
            }
            const float x00 = Lerp(corner[0], corner[1], u);
            const float x10 = Lerp(corner[2], corner[3], u);
            const float x01 = Lerp(corner[4], corner[5], u);
            const float x11 = Lerp(corner[6], corner[7], u);
            return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w);
        }

        void VectorNoise(const float q[3], float out[3])
        {
            for (uint32_t channel = 0; channel < 3; ++channel) {
                out[channel] = GradientNoise(q[0], q[1], q[2], channel);
            }
        }
    }

    float CurlNoiseRamp(float r)
    {
        if (r >= 1.0f) {
            return 1.0f;
        }
        if (r <= -1.0f) {
            return -1.0f;
        }
        const float r2 = r * r;
        return r * (15.0f / 8.0f - 10.0f / 8.0f * r2 + 3.0f / 8.0f * r2 * r2);
    }

    SdfWindField::SdfWindField(const SignedDistanceGrid& field, const VoxelGridDesc& desc, const SdfWindSettings& settings)
        : m_field(field)
        , m_desc(desc)
        , m_settings(settings)
    {
        const float dims[3] = { static_cast<float>(desc.dimX), static_cast<float>(desc.dimY),
                                static_cast<float>(desc.dimZ) };
        for (int axis = 0; axis < 3; ++axis) {
            m_center[axis] = desc.origin[axis] + 0.5f * desc.voxelSize * dims[axis];
        }
    }

    float SdfWindField::Distance(const float position[3]) const
    {
        const float inv = 1.0f / m_desc.voxelSize;
        const float vx = (position[0] - m_desc.origin[0]) * inv;
        const float vy = (position[1] - m_desc.origin[1]) * inv;
        const float vz = (position[2] - m_desc.origin[2]) * inv;
        return SampleSdfTrilinear(m_field, vx, vy, vz) * m_desc.voxelSize;
    }

    void SdfWindField::Normal(const float position[3], float outNormal[3]) const
    {
        const float h = 0.5f * m_desc.voxelSize;
        float n[3];
        for (int axis = 0; axis < 3; ++axis) {
            float plus[3] = { position[0], position[1], position[2] };
            float minus[3] = { position[0], position[1], position[2] };
            plus[axis] += h;
            minus[axis] -= h;
            n[axis] = Distance(plus) - Distance(minus);
        }
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length < 1.0e-6f) {
            outNormal[0] = outNormal[1] = outNormal[2] = 0.0f;
            return;
        }
        const float invLength = 1.0f / length;
        outNormal[0] = n[0] * invLength;
        outNormal[1] = n[1] * invLength;
        outNormal[2] = n[2] * invLength;
    }

    void SdfWindField::Gauge(const float position[3], float outOrigin[3]) const
    {
        outOrigin[0] = m_center[0];
        outOrigin[1] = m_center[1];
        outOrigin[2] = m_center[2];
        // Without the laminar term the gauge does not enter psi.
        if (!m_settings.laminar || m_settings.rampWidth <= 0.0f) {
            return;
        }
        const float distance = Distance(position);
        if (std::fabs(distance) >= m_settings.rampWidth) {
            return;
        }
        float n[3];
        Normal(position, n);
        for (int axis = 0; axis < 3; ++axis) {
            outOrigin[axis] = position[axis] - distance * n[axis];
        }
    }

    void SdfWindField::Potential(const float position[3], float outPsi[3]) const
    {
        float origin[3];
        Gauge(position, origin);
        PotentialWithGauge(position, origin, outPsi);
    }

    void SdfWindField::PotentialWithGauge(const float position[3], const float origin[3], float outPsi[3]) const
    {
        const float* wind = m_settings.wind;
        const float rel[3] = { position[0] - origin[0], position[1] - origin[1], position[2] - origin[2] };
        float psi[3] = { 0.0f, 0.0f, 0.0f };
        if (m_settings.laminar) {
            psi[0] = 0.5f * (wind[1] * rel[2] - wind[2] * rel[1]);
            psi[1] = 0.5f * (wind[2] * rel[0] - wind[0] * rel[2]);
            psi[2] = 0.5f * (wind[0] * rel[1] - wind[1] * rel[0]);
        }

        if (m_settings.turbulence != 0.0f && m_settings.noiseScale > 0.0f) {
            const float windLength = std::sqrt(wind[0] * wind[0] + wind[1] * wind[1] + wind[2] * wind[2]);
            const float invScale = 1.0f / m_settings.noiseScale;
            const float q[3] = {
                (position[0] - m_settings.time * wind[0]) * invScale,
                (position[1] - m_settings.time * wind[1]) * invScale,
                (position[2] - m_settings.time * wind[2]) * invScale,
            };
            float noise[3];
            VectorNoise(q, noise);
            const float amplitude = m_settings.turbulence * windLength * m_settings.noiseScale;
            for (int axis = 0; axis < 3; ++axis) {
                psi[axis] += amplitude * noise[axis];
            }
        }

        float blend = 1.0f;
        if (m_settings.rampWidth > 0.0f) {
            blend = std::fabs(CurlNoiseRamp(Distance(position) / m_settings.rampWidth));
        }
        if (blend >= 1.0f) {
            outPsi[0] = psi[0];
            outPsi[1] = psi[1];
            outPsi[2] = psi[2];
            return;
        }

        float n[3];
        Normal(position, n);
        const float nDotPsi = n[0] * psi[0] + n[1] * psi[1] + n[2] * psi[2];
        for (int axis = 0; axis < 3; ++axis) {
            outPsi[axis] = blend * psi[axis] + (1.0f - blend) * n[axis] * nDotPsi;
        }
    }

    void SdfWindField::Velocity(const float position[3], float outVelocity[3]) const
    {
        const float h = 0.5f * m_desc.voxelSize;
        // One gauge for the whole stencil, so the differences see a single potential.
        float origin[3];
        Gauge(position, origin);
        // d[axis][component] = d psi_component / d axis
        float d[3][3];
        for (int axis = 0; axis < 3; ++axis) {
            float plus[3] = { position[0], position[1], position[2] };
            float minus[3] = { position[0], position[1], position[2] };
            plus[axis] += h;
            minus[axis] -= h;
            float psiPlus[3];
            float psiMinus[3];
            PotentialWithGauge(plus, origin, psiPlus);
            PotentialWithGauge(minus, origin, psiMinus);
            for (int c = 0; c < 3; ++c) {
                d[axis][c] = (psiPlus[c] - psiMinus[c]) / (2.0f * h);
            }
        }
        float v[3] = {
            d[1][2] - d[2][1],
            d[2][0] - d[0][2],
            d[0][1] - d[1][0],
        };

        const float* wind = m_settings.wind;
        const float windLength = std::sqrt(wind[0] * wind[0] + wind[1] * wind[1] + wind[2] * wind[2]);
        if (windLength > 0.0f && m_settings.maxSpeedScale > 0.0f) {
            const float maxSpeed = m_settings.maxSpeedScale * windLength;
            const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (speed > maxSpeed) {
                const float scale = maxSpeed / speed;
                v[0] *= scale;
                v[1] *= scale;
                v[2] *= scale;
            }
        }
        outVelocity[0] = v[0];
        outVelocity[1] = v[1];
        outVelocity[2] = v[2];
    }

    bool SdfWindField::Contains(const float position[3]) const
    {
        const float dims[3] = { static_cast<float>(m_desc.dimX), static_cast<float>(m_desc.dimY),
                                static_cast<float>(m_desc.dimZ) };
        for (int axis = 0; axis < 3; ++axis) {
            const float low = m_desc.origin[axis];
            const float high = low + dims[axis] * m_desc.voxelSize;
            if (position[axis] < low || position[axis] > high) {
                return false;
            }
        }
        return true;
    }
}
