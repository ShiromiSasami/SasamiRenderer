#include "FlipDiffuseMesh.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr uint32_t kVerticesPerParticle = 6;
        constexpr uint32_t kBoundsVertices = 2;
        constexpr float kBoundsMargin = 1.5f;  // m, covers the hidden point under the floor
        constexpr float kSprayHeadroom = 6.0f; // m above the grid top for spray
        // +x, -x, +y, -y, +z, -z
        constexpr float kDirections[kVerticesPerParticle][3] = {
            { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
        };
        // Counter-clockwise seen from outside, matching the height-field surface winding.
        constexpr uint32_t kFaces[8][3] = {
            { 2, 0, 4 }, { 2, 4, 1 }, { 2, 1, 5 }, { 2, 5, 0 },
            { 3, 4, 0 }, { 3, 1, 4 }, { 3, 5, 1 }, { 3, 0, 5 },
        };

        // Radius (m) and colour per DiffuseKind. Picked by eye for the h = 0.5 m tank.
        constexpr float kRadius[3] = { 0.06f, 0.16f, 0.05f };
        constexpr float kColor[3][3] = {
            { 0.95f, 0.97f, 1.0f },   // spray
            { 0.92f, 0.95f, 0.96f },  // foam
            { 0.62f, 0.84f, 0.92f },  // bubble
        };
        // Foam shrinks over its last second instead of popping out.
        constexpr float kFadeTime = 1.0f;
    }

    void FlipDiffuseMesh::Configure()
    {
        // Two extra vertices after the last slot are never indexed; Update() puts them on the tank
        // corners so the bounds the submitter computes once at upload cover every particle.
        m_mesh.vertices.assign(size_t(kMaxDrawn) * kVerticesPerParticle + kBoundsVertices, Vertex{});
        m_mesh.indices.clear();
        m_mesh.indices.reserve(size_t(kMaxDrawn) * 24);
        for (uint32_t p = 0; p < kMaxDrawn; ++p)
        {
            const uint32_t base = p * kVerticesPerParticle;
            for (const auto& f : kFaces)
            {
                m_mesh.indices.insert(m_mesh.indices.end(), { base + f[0], base + f[1], base + f[2] });
            }
        }
        for (uint32_t i = 0; i < m_mesh.vertices.size(); ++i)
        {
            const float* d = kDirections[i % kVerticesPerParticle];
            Vertex& v = m_mesh.vertices[i];
            for (int c = 0; c < 3; ++c)
            {
                v.normal[c] = d[c];
            }
            v.color[3] = 1.0f;
        }
        m_drawn = 0;
    }

    void FlipDiffuseMesh::Update(const FlipWaterSolver& solver, const FlipDiffuseParticles& diffuse)
    {
        if (m_mesh.vertices.empty())
        {
            Configure();
        }
        const FlipWaterSettings& s = solver.Settings();
        const float hidden[3] = { s.origin[0], s.origin[1] - 1.0f, s.origin[2] };
        const float extent[3] = { float(s.nx) * s.cellSize, float(s.ny) * s.cellSize, float(s.nz) * s.cellSize };
        Vertex* anchor = &m_mesh.vertices[size_t(kMaxDrawn) * kVerticesPerParticle];
        for (int c = 0; c < 3; ++c)
        {
            anchor[0].position[c] = s.origin[c] - kBoundsMargin;
            anchor[1].position[c] = s.origin[c] + extent[c] + kBoundsMargin;
        }
        anchor[1].position[1] += kSprayHeadroom;

        const uint32_t count = diffuse.Count();
        const uint32_t stride = (std::max)(1u, (count + kMaxDrawn - 1) / kMaxDrawn);
        const std::vector<float>& x = diffuse.Positions();
        const std::vector<uint8_t>& kind = diffuse.Kinds();
        const std::vector<float>& life = diffuse.Lifetimes();

        uint32_t slot = 0;
        m_visible = 0;
        for (uint32_t p = 0; p < count && slot < kMaxDrawn; p += stride, ++slot)
        {
            const uint32_t k = (std::min)(uint32_t(kind[p]), 2u);
            float r = kRadius[k] * m_sizeScale;
            if (k == uint32_t(DiffuseKind::Foam))
            {
                r *= std::clamp(life[p] / kFadeTime, 0.0f, 1.0f);
            }
            float centre[3] = { x[size_t(p) * 3 + 0], x[size_t(p) * 3 + 1], x[size_t(p) * 3 + 2] };
            if (k == uint32_t(DiffuseKind::Foam))
            {
                // Foam sits in the top particle layer, which is under the drawn height field (the
                // highest particle plus half a spacing, smoothed), so lift it onto that surface.
                centre[1] = (std::max)(centre[1], solver.SampleSurfaceHeight(centre[0], centre[2]));
            }
            if (centre[1] + r >= solver.SampleSurfaceHeight(centre[0], centre[2]))
            {
                ++m_visible;
            }
            Vertex* v = &m_mesh.vertices[size_t(slot) * kVerticesPerParticle];
            for (uint32_t i = 0; i < kVerticesPerParticle; ++i)
            {
                for (int c = 0; c < 3; ++c)
                {
                    v[i].position[c] = centre[c] + kDirections[i][c] * r;
                    v[i].color[c] = kColor[k][c];
                }
            }
        }
        m_drawn = slot;
        for (; slot < kMaxDrawn; ++slot)
        {
            Vertex* v = &m_mesh.vertices[size_t(slot) * kVerticesPerParticle];
            if (v[0].position[0] == hidden[0] && v[0].position[1] == hidden[1] && v[0].position[2] == hidden[2]
                && v[1].position[1] == hidden[1])
            {
                // Already collapsed, and so is every later slot written by the previous Update.
                break;
            }
            for (uint32_t i = 0; i < kVerticesPerParticle; ++i)
            {
                for (int c = 0; c < 3; ++c)
                {
                    v[i].position[c] = hidden[c];
                }
            }
        }
    }
}
