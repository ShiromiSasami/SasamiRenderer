#include "Renderer/Voxel/WindDrivenParticles.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr float TWO_PI = 6.28318530717958647692f;
        constexpr uint32_t MAX_SPHERE_TRIES = 8;
        constexpr float MIN_DRAG_TIME = 1e-4f;
        constexpr float MIN_LIFETIME = 1e-3f;
        constexpr float PUSH_OUT_VOXELS = 0.05f;

        void SwapPop(std::vector<float>& values, size_t index, size_t stride)
        {
            const size_t last = values.size() / stride - 1;
            if (index != last) {
                for (size_t c = 0; c < stride; ++c) {
                    values[index * stride + c] = values[last * stride + c];
                }
            }
            values.resize(last * stride);
        }
    }

    WindParticleStepParams MakeWindParticleStepParams(const WindDrivenParticleSettings& settings, float voxelSize, float dt)
    {
        WindParticleStepParams params;
        params.dt = dt;
        params.tau = (std::max)(settings.dragTime, MIN_DRAG_TIME);
        params.decay = std::exp(-dt / params.tau);
        params.voxelSize = voxelSize;
        for (int c = 0; c < 3; ++c) {
            params.acceleration[c] = settings.acceleration[c];
        }
        params.buoyancy = settings.buoyancy;
        params.flutterAcceleration = settings.flutterAcceleration;
        params.flutterFrequency = settings.flutterFrequency;
        params.wallFriction = settings.wallFriction;
        params.flutter = (settings.kind == WindParticleKind::Petal && settings.flutterAcceleration > 0.0f) ? 1u : 0u;
        return params;
    }

    // CPU reference for Shaders/Compute/Voxel/WindParticleStep_CS.hlsl.
    // A particle that is already dead (alive == 0) is left untouched and reported as Expired.
    WindParticleFate StepWindParticle(WindParticleState& state, const IWindVelocityField& wind,
                                      const WindParticleStepParams& params)
    {
        if (state.alive == 0) {
            return WindParticleFate::Expired;
        }

        float* p = state.position;
        float* v = state.velocity;

        float u[3] = { 0.0f, 0.0f, 0.0f };
        if (wind.Contains(p)) {
            wind.Velocity(p, u);
        }

        const float age01 = (std::min)((std::max)(state.age / state.lifetime, 0.0f), 1.0f);
        float a[3] = { params.acceleration[0],
                       params.acceleration[1] + params.buoyancy * (1.0f - age01),
                       params.acceleration[2] };
        if (params.flutter != 0) {
            state.phase += TWO_PI * params.flutterFrequency * params.dt;
            const float side = params.flutterAcceleration * std::sin(state.phase);
            const float axis[3] = { state.flutterAxisX, 0.0f, state.flutterAxisZ };
            for (int c = 0; c < 3; ++c) {
                a[c] += axis[c] * side;
            }
        }
        state.angle += state.spin * params.dt;

        for (int c = 0; c < 3; ++c) {
            const float target = u[c] + a[c] * params.tau;
            const float offset = v[c] - target;
            p[c] += target * params.dt + offset * params.tau * (1.0f - params.decay);
            v[c] = target + offset * params.decay;
        }
        state.age += params.dt;

        const float d = wind.Distance(p);
        if (d < 0.0f) {
            ++state.wallHits;
            const float h = 0.5f * params.voxelSize;
            float n[3];
            for (int c = 0; c < 3; ++c) {
                float hi[3] = { p[0], p[1], p[2] };
                float lo[3] = { p[0], p[1], p[2] };
                hi[c] += h;
                lo[c] -= h;
                n[c] = wind.Distance(hi) - wind.Distance(lo);
            }
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-6f) {
                n[0] = 0.0f;
                n[1] = 1.0f;
                n[2] = 0.0f;
            } else {
                for (int c = 0; c < 3; ++c) {
                    n[c] /= len;
                }
            }
            const float push = -d + PUSH_OUT_VOXELS * params.voxelSize;
            for (int c = 0; c < 3; ++c) {
                p[c] += n[c] * push;
            }
            const float vn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
            if (vn < 0.0f) {
                for (int c = 0; c < 3; ++c) {
                    v[c] -= vn * n[c];
                }
            }
            for (int c = 0; c < 3; ++c) {
                v[c] *= params.wallFriction;
            }
        }

        if (state.age >= state.lifetime) {
            state.alive = 0;
            return WindParticleFate::Expired;
        }
        if (!wind.Contains(p)) {
            state.alive = 0;
            return WindParticleFate::Escaped;
        }
        return WindParticleFate::Alive;
    }

    void WindDrivenParticles::Reset(const WindDrivenParticleSettings& settings, uint32_t seed)
    {
        m_settings = settings;
        m_stats = WindDrivenParticleStats();
        m_rng = seed ? seed : 1u;
        m_spawnCarry = 0.0f;
        m_position.clear();
        m_velocity.clear();
        m_age.clear();
        m_lifetime.clear();
        m_angle.clear();
        m_spin.clear();
        m_phase.clear();
        m_flutterAxis.clear();
    }

    void WindDrivenParticles::SetEmitterPosition(const float position[3])
    {
        for (int c = 0; c < 3; ++c) {
            m_settings.emitterPosition[c] = position[c];
        }
    }

    float WindDrivenParticles::Random01()
    {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return static_cast<float>(m_rng >> 8) * (1.0f / 16777216.0f);
    }

    void WindDrivenParticles::SpawnOne(const float centre[3], float radius)
    {
        if (Count() >= m_settings.maxParticles) {
            ++m_stats.dropped;
            return;
        }

        float offset[3] = { 0.0f, 0.0f, 0.0f };
        for (uint32_t t = 0; t < MAX_SPHERE_TRIES; ++t) {
            const float x = 2.0f * Random01() - 1.0f;
            const float y = 2.0f * Random01() - 1.0f;
            const float z = 2.0f * Random01() - 1.0f;
            if (x * x + y * y + z * z <= 1.0f) {
                offset[0] = x;
                offset[1] = y;
                offset[2] = z;
                break;
            }
        }
        for (int c = 0; c < 3; ++c) {
            m_position.push_back(centre[c] + offset[c] * radius);
        }
        for (int c = 0; c < 3; ++c) {
            m_velocity.push_back(m_settings.initialVelocity[c] +
                                 m_settings.velocityJitter * (2.0f * Random01() - 1.0f));
        }

        const float lifetime = m_settings.minLifetime + (m_settings.maxLifetime - m_settings.minLifetime) * Random01();
        m_age.push_back(0.0f);
        m_lifetime.push_back((std::max)(lifetime, MIN_LIFETIME));
        m_angle.push_back(TWO_PI * Random01());
        m_spin.push_back(m_settings.spinRate * (2.0f * Random01() - 1.0f));
        m_phase.push_back(TWO_PI * Random01());
        const float axisAngle = TWO_PI * Random01();
        m_flutterAxis.push_back(std::cos(axisAngle));
        m_flutterAxis.push_back(0.0f);
        m_flutterAxis.push_back(std::sin(axisAngle));
        ++m_stats.spawned;
    }

    void WindDrivenParticles::Emit(const float position[3], float radius, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i) {
            SpawnOne(position, radius);
        }
    }

    void WindDrivenParticles::Remove(uint32_t i)
    {
        SwapPop(m_position, i, 3);
        SwapPop(m_velocity, i, 3);
        SwapPop(m_flutterAxis, i, 3);
        SwapPop(m_age, i, 1);
        SwapPop(m_lifetime, i, 1);
        SwapPop(m_angle, i, 1);
        SwapPop(m_spin, i, 1);
        SwapPop(m_phase, i, 1);
    }

    void WindDrivenParticles::Step(const IWindVelocityField& wind, float dt)
    {
        if (dt <= 0.0f) {
            m_stats.alive = Count();
            return;
        }

        if (m_settings.spawnRate > 0.0f) {
            m_spawnCarry += m_settings.spawnRate * dt;
            const float whole = std::floor(m_spawnCarry);
            m_spawnCarry -= whole;
            Emit(m_settings.emitterPosition, m_settings.emitterRadius, static_cast<uint32_t>(whole));
        }

        const WindParticleStepParams params = MakeWindParticleStepParams(m_settings, wind.Desc().voxelSize, dt);

        uint32_t i = 0;
        while (i < Count()) {
            float* p = &m_position[3 * static_cast<size_t>(i)];
            float* v = &m_velocity[3 * static_cast<size_t>(i)];

            WindParticleState state = {};
            for (int c = 0; c < 3; ++c) {
                state.position[c] = p[c];
                state.velocity[c] = v[c];
            }
            state.age = m_age[i];
            state.lifetime = m_lifetime[i];
            state.angle = m_angle[i];
            state.spin = m_spin[i];
            state.phase = m_phase[i];
            state.alive = 1;
            state.flutterAxisX = m_flutterAxis[3 * static_cast<size_t>(i)];
            state.flutterAxisZ = m_flutterAxis[3 * static_cast<size_t>(i) + 2];
            state.wallHits = 0;

            const WindParticleFate fate = StepWindParticle(state, wind, params);

            for (int c = 0; c < 3; ++c) {
                p[c] = state.position[c];
                v[c] = state.velocity[c];
            }
            m_age[i] = state.age;
            m_angle[i] = state.angle;
            m_phase[i] = state.phase;
            m_stats.wallHits += state.wallHits;

            if (fate == WindParticleFate::Expired) {
                ++m_stats.expired;
                Remove(i);
            } else if (fate == WindParticleFate::Escaped) {
                ++m_stats.escaped;
                Remove(i);
            } else {
                ++i;
            }
        }
        m_stats.alive = Count();
    }

    void WindDrivenParticles::ExportStates(std::vector<WindParticleState>& outStates) const
    {
        outStates.clear();
        const uint32_t count = Count();
        outStates.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            WindParticleState state = {};
            for (int c = 0; c < 3; ++c) {
                state.position[c] = m_position[3 * static_cast<size_t>(i) + c];
                state.velocity[c] = m_velocity[3 * static_cast<size_t>(i) + c];
            }
            state.age = m_age[i];
            state.lifetime = m_lifetime[i];
            state.angle = m_angle[i];
            state.spin = m_spin[i];
            state.phase = m_phase[i];
            state.alive = 1;
            state.flutterAxisX = m_flutterAxis[3 * static_cast<size_t>(i)];
            state.flutterAxisZ = m_flutterAxis[3 * static_cast<size_t>(i) + 2];
            state.wallHits = 0;
            state.pad = 0;
            outStates.push_back(state);
        }
    }

    void WindDrivenParticles::ClearParticles()
    {
        m_position.clear();
        m_velocity.clear();
        m_age.clear();
        m_lifetime.clear();
        m_angle.clear();
        m_spin.clear();
        m_phase.clear();
        m_flutterAxis.clear();
    }

    void WindDrivenParticles::ImportStates(const std::vector<WindParticleState>& states)
    {
        ClearParticles();
        for (const WindParticleState& state : states) {
            if (state.alive == 0) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                m_position.push_back(state.position[c]);
                m_velocity.push_back(state.velocity[c]);
            }
            m_age.push_back(state.age);
            m_lifetime.push_back(state.lifetime);
            m_angle.push_back(state.angle);
            m_spin.push_back(state.spin);
            m_phase.push_back(state.phase);
            m_flutterAxis.push_back(state.flutterAxisX);
            m_flutterAxis.push_back(0.0f);
            m_flutterAxis.push_back(state.flutterAxisZ);
        }
        m_stats.alive = Count();
    }

    float WindDrivenParticles::Size(uint32_t i) const
    {
        const float t = (std::min)((std::max)(Age01(i), 0.0f), 1.0f);
        return m_settings.startSize + (m_settings.endSize - m_settings.startSize) * t;
    }

    float WindDrivenParticles::Facing(uint32_t i) const
    {
        if (m_settings.kind == WindParticleKind::Petal) {
            return std::fabs(std::cos(m_phase[i]));
        }
        return 1.0f;
    }
}
