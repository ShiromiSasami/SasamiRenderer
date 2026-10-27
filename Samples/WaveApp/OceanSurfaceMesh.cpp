#include "OceanSurfaceMesh.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Sample::Wave
{
    void OceanSurfaceMesh::Configure(const OceanSurfaceSettings& settings, SeaBedFunction seaBed)
    {
        m_settings = settings;
        m_settings.resolution = (std::max)(m_settings.resolution, 2u);
        m_seaBed = seaBed;

        const uint32_t n = m_settings.resolution;
        const uint32_t side = n + 1;
        m_mesh.vertices.assign(side * side, Vertex{});
        m_mesh.indices.clear();
        m_mesh.indices.reserve(n * n * 6);
        for (uint32_t z = 0; z < n; ++z)
        {
            for (uint32_t x = 0; x < n; ++x)
            {
                const uint32_t i0 = z * side + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + side;
                const uint32_t i3 = i2 + 1;
                // Clockwise seen from +y, matching the engine's other up-facing meshes (AddBox top face).
                m_mesh.indices.insert(m_mesh.indices.end(), { i0, i2, i1, i1, i2, i3 });
            }
        }
        m_foam.assign(side * side, 0.0f);
        m_stats = {};
    }

    void OceanSurfaceMesh::Update(const GerstnerWaveSet& waves, float time, float dt, const CoastalWindField* wind)
    {
        const uint32_t side = m_settings.resolution + 1;
        if (m_mesh.vertices.size() != size_t(side) * side)
        {
            return;
        }
        const float step = m_settings.size / static_cast<float>(m_settings.resolution);
        const float origin = -0.5f * m_settings.size;
        const float decay = std::exp(-(std::max)(dt, 0.0f) / (std::max)(m_settings.foamDecay, 1e-3f));
        const float jRange = (std::max)(m_settings.foamJacobian, 1e-3f);

        OceanSurfaceStats stats{};
        stats.vertices = side * side;
        stats.minJacobian = 1e9f;
        stats.maxHeight = -1e9f;
        double gustSum = 0.0;

        for (uint32_t zi = 0; zi < side; ++zi)
        {
            for (uint32_t xi = 0; xi < side; ++xi)
            {
                const uint32_t index = zi * side + xi;
                const float x = origin + step * xi;
                const float z = origin + step * zi;
                const float bed = m_seaBed ? m_seaBed(x, z) : -100.0f;
                const float depth = -bed;
                const float shoal = std::clamp(depth / (std::max)(m_settings.shoalDepth, 1e-3f), 0.15f, 1.0f);

                const float gust = wind ? wind->Sample(x, z, time).gust : 1.0f;
                gustSum += gust;
                const GerstnerSample s = waves.Evaluate(x, z, time, shoal, gust);
                stats.minJacobian = (std::min)(stats.minJacobian, s.jacobian);
                stats.maxHeight = (std::max)(stats.maxHeight, s.position[1]);

                // Whitecap: 0 at J = foamJacobian, 1 at J <= 0 (folded crest).
                float source = std::clamp((m_settings.foamJacobian - s.jacobian) / jRange, 0.0f, 1.0f);
                if (depth > 0.0f && depth < m_settings.shoreFoamDepth)
                {
                    // Surf line: strongest where the water is thinnest, modulated by the local wave height.
                    const float shore = 1.0f - depth / m_settings.shoreFoamDepth;
                    const float crest = std::clamp(0.5f + s.position[1] / (std::max)(0.05f, 0.6f * shoal), 0.0f, 1.0f);
                    source = (std::max)(source, shore * crest);
                }
                float& foam = m_foam[index];
                foam = (std::max)(foam * decay, source);
                if (foam > 0.5f)
                {
                    ++stats.foamVertices;
                }

                Vertex& v = m_mesh.vertices[index];
                v.position[0] = s.position[0];
                v.position[1] = s.position[1];
                v.position[2] = s.position[2];
                if (depth <= 0.0f)
                {
                    // Dry: sink below the bed so the beach mesh covers it.
                    v.position[1] = bed - 0.2f;
                }
                v.normal[0] = s.normal[0];
                v.normal[1] = s.normal[1];
                v.normal[2] = s.normal[2];
                for (int c = 0; c < 3; ++c)
                {
                    v.color[c] = m_settings.waterColor[c] + (m_settings.foamColor[c] - m_settings.waterColor[c]) * foam;
                }
                v.color[3] = 1.0f;
                v.uv[0] = static_cast<float>(xi) / static_cast<float>(m_settings.resolution);
                v.uv[1] = static_cast<float>(zi) / static_cast<float>(m_settings.resolution);
            }
        }
        stats.meanGust = static_cast<float>(gustSum / (std::max)(stats.vertices, 1u));
        m_stats = stats;
    }
}
