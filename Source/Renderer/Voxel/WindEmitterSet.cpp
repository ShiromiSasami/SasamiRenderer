#include "Renderer/Voxel/WindEmitterSet.h"

#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr float MIN_HORIZONTAL = 1e-4f;
    }

    void WindEmitterSet::Add(const float position[3])
    {
        m_position.insert(m_position.end(), position, position + 3);
        m_carry.push_back(0.0f);
    }

    bool WindEmitterSet::RemoveLast()
    {
        if (m_carry.empty()) {
            return false;
        }
        m_carry.pop_back();
        m_position.resize(m_position.size() - 3);
        return true;
    }

    void WindEmitterSet::Emit(WindDrivenParticles& particles, float rate, float dt, const float offset[3], float radius)
    {
        if (rate <= 0.0f || dt <= 0.0f) {
            return;
        }
        for (uint32_t i = 0; i < Count(); ++i) {
            m_carry[i] += rate * dt;
            const float whole = std::floor(m_carry[i]);
            m_carry[i] -= whole;
            const float* p = Position(i);
            const float centre[3] = { p[0] + offset[0], p[1] + offset[1], p[2] + offset[2] };
            particles.Emit(centre, radius, static_cast<uint32_t>(whole));
        }
    }

    namespace
    {
        float HorizontalCosine(const float a[3], const float b[3])
        {
            const float aLen = std::sqrt(a[0] * a[0] + a[2] * a[2]);
            const float bLen = std::sqrt(b[0] * b[0] + b[2] * b[2]);
            if (aLen <= MIN_HORIZONTAL || bLen <= MIN_HORIZONTAL) {
                return 0.0f;
            }
            return (a[0] * b[0] + a[2] * b[2]) / (aLen * bLen);
        }
    }

    std::vector<WindSourceDrift> MeasureWindSourceDrift(const WindEmitterSet& sources, const float offset[3],
                                                        const std::vector<float>& points, const IWindVelocityField& wind)
    {
        std::vector<WindSourceDrift> drift(sources.Count());
        if (drift.empty()) {
            return drift;
        }
        for (size_t k = 0; k + 2 < points.size(); k += 3) {
            const float* q = &points[k];
            uint32_t best = 0;
            float bestDist = 0.0f;
            for (uint32_t i = 0; i < sources.Count(); ++i) {
                const float* p = sources.Position(i);
                const float dx = q[0] - p[0];
                const float dy = q[1] - p[1];
                const float dz = q[2] - p[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (i == 0 || d < bestDist) {
                    best = i;
                    bestDist = d;
                }
            }
            const float* p = sources.Position(best);
            WindSourceDrift& out = drift[best];
            float u[3] = { 0.0f, 0.0f, 0.0f };
            if (wind.Contains(q)) {
                wind.Velocity(q, u);
            }
            for (int a = 0; a < 3; ++a) {
                out.meanOffset[a] += q[a] - p[a];
                out.pathWind[a] += u[a];
            }
            ++out.points;
        }
        for (uint32_t i = 0; i < sources.Count(); ++i) {
            WindSourceDrift& out = drift[i];
            if (out.points > 0) {
                for (int a = 0; a < 3; ++a) {
                    out.meanOffset[a] /= static_cast<float>(out.points);
                    out.pathWind[a] /= static_cast<float>(out.points);
                }
            }
            const float* p = sources.Position(i);
            const float at[3] = { p[0] + offset[0], p[1] + offset[1], p[2] + offset[2] };
            if (wind.Contains(at)) {
                wind.Velocity(at, out.sourceWind);
            }
            out.cosine = HorizontalCosine(out.meanOffset, out.pathWind);
            out.sourceCosine = HorizontalCosine(out.meanOffset, out.sourceWind);
        }
        return drift;
    }

    void CollectParticlePositions(const WindDrivenParticles& particles, std::vector<float>& outPoints)
    {
        outPoints.clear();
        outPoints.reserve(3 * static_cast<size_t>(particles.Count()));
        for (uint32_t i = 0; i < particles.Count(); ++i) {
            const float* p = particles.Position(i);
            outPoints.insert(outPoints.end(), p, p + 3);
        }
    }
}
