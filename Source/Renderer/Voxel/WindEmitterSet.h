#pragma once

#include "Renderer/Voxel/WindDrivenParticles.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Several point sources feeding one WindDrivenParticles system (whose own continuous emitter is
    // left at rate 0). Each source emits `rate` particles per second inside a sphere of `radius`
    // around position + offset, with its own fractional carry.
    class WindEmitterSet
    {
    public:
        void Clear() { m_position.clear(); m_carry.clear(); }
        void Add(const float position[3]);
        bool RemoveLast();
        uint32_t Count() const { return static_cast<uint32_t>(m_carry.size()); }
        const float* Position(uint32_t i) const { return &m_position[3 * static_cast<size_t>(i)]; }

        void Emit(WindDrivenParticles& particles, float rate, float dt, const float offset[3], float radius);

    private:
        std::vector<float> m_position;
        std::vector<float> m_carry;
    };

    // How well what a source produced follows the wind: the points (xyz triples, e.g. live particles
    // or burnt cells) are assigned to their nearest source, and the mean horizontal (xz) offset from
    // that source is compared with
    //   pathWind   - the wind averaged over those same points (what actually carried them), and
    //   sourceWind - the wind at source + offset (one sample; near walls it can be almost calm).
    // A cosine is 1 when the cloud has drifted straight downwind, 0 when it has no xz offset or the
    // wind is calm.
    struct WindSourceDrift
    {
        uint32_t points = 0;
        float meanOffset[3] = { 0.0f, 0.0f, 0.0f };
        float pathWind[3] = { 0.0f, 0.0f, 0.0f };
        float sourceWind[3] = { 0.0f, 0.0f, 0.0f };
        float cosine = 0.0f;          // meanOffset vs pathWind
        float sourceCosine = 0.0f;    // meanOffset vs sourceWind
    };

    std::vector<WindSourceDrift> MeasureWindSourceDrift(const WindEmitterSet& sources, const float offset[3],
                                                        const std::vector<float>& points, const IWindVelocityField& wind);

    // Live particle positions as xyz triples.
    void CollectParticlePositions(const WindDrivenParticles& particles, std::vector<float>& outPoints);
}
