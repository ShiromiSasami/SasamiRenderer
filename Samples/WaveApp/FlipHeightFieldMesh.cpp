#include "FlipHeightFieldMesh.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    void FlipHeightFieldMesh::Configure(const FlipWaterSolver& solver, const float waterColor[3])
    {
        const FlipWaterSettings& s = solver.Settings();
        m_nx = s.nx;
        m_nz = s.nz;
        for (int c = 0; c < 3; ++c)
        {
            m_waterColor[c] = waterColor[c];
        }
        const uint32_t sideX = m_nx + 1;
        const uint32_t sideZ = m_nz + 1;
        m_mesh.vertices.assign(size_t(sideX) * sideZ, Vertex{});
        m_mesh.indices.clear();
        m_mesh.indices.reserve(size_t(m_nx) * m_nz * 6);
        for (uint32_t z = 0; z < m_nz; ++z)
        {
            for (uint32_t x = 0; x < m_nx; ++x)
            {
                const uint32_t i0 = z * sideX + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + sideX;
                const uint32_t i3 = i2 + 1;
                m_mesh.indices.insert(m_mesh.indices.end(), { i0, i2, i1, i1, i2, i3 });
            }
        }

        m_surfaceVertexCount = sideX * sideZ;
        m_skirts.assign(4, SkirtEdge{});
        for (uint32_t xi = 0; xi < sideX; ++xi)
        {
            m_skirts[0].corners.push_back(m_nx - xi);                    // z = 0 side, normal -z, d = -x
            m_skirts[1].corners.push_back(m_nz * sideX + xi);            // z = nz side, normal +z, d = +x
        }
        for (uint32_t zi = 0; zi < sideZ; ++zi)
        {
            m_skirts[2].corners.push_back(zi * sideX);                   // x = 0 side, normal -x, d = +z
            m_skirts[3].corners.push_back((m_nz - zi) * sideX + m_nx);   // x = nx side, normal +x, d = -z
        }
        const float normals[4][3] = { { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
        uint32_t next = m_surfaceVertexCount;
        for (uint32_t e = 0; e < 4; ++e)
        {
            SkirtEdge& edge = m_skirts[e];
            for (int c = 0; c < 3; ++c)
            {
                edge.normal[c] = normals[e][c];
            }
            // Two vertices per corner: top (at the surface) then bottom (at the bed).
            const uint32_t count = static_cast<uint32_t>(edge.corners.size());
            for (uint32_t i = 0; i + 1 < count; ++i)
            {
                const uint32_t ta = next + 2 * i;
                const uint32_t ba = ta + 1;
                const uint32_t tb = ta + 2;
                const uint32_t bb = ta + 3;
                m_mesh.indices.insert(m_mesh.indices.end(), { ta, ba, tb, tb, ba, bb });
            }
            next += 2 * count;
        }
        m_mesh.vertices.resize(next, Vertex{});
        Update(solver);
    }

    void FlipHeightFieldMesh::Update(const FlipWaterSolver& solver)
    {
        const FlipWaterSettings& s = solver.Settings();
        const uint32_t sideX = m_nx + 1;
        const uint32_t sideZ = m_nz + 1;
        if (s.nx != m_nx || s.nz != m_nz || m_surfaceVertexCount != sideX * sideZ)
        {
            return;
        }
        const std::vector<float>& heights = solver.ColumnHeights();
        const std::vector<uint8_t>& wet = solver.ColumnWet();
        if (heights.size() != size_t(m_nx) * m_nz || wet.size() != heights.size())
        {
            return;
        }
        const float h = s.cellSize;
        std::vector<float> cornerY(size_t(sideX) * sideZ, 0.0f);
        std::vector<uint8_t> cornerWet(cornerY.size(), 0);
        m_wetVertices = 0;
        for (uint32_t zi = 0; zi < sideZ; ++zi)
        {
            for (uint32_t xi = 0; xi < sideX; ++xi)
            {
                float sum = 0.0f;
                uint32_t count = 0;
                for (int dz = -1; dz <= 0; ++dz)
                {
                    for (int dx = -1; dx <= 0; ++dx)
                    {
                        const int cx = int(xi) + dx;
                        const int cz = int(zi) + dz;
                        if (cx < 0 || cz < 0 || cx >= int(m_nx) || cz >= int(m_nz))
                        {
                            continue;
                        }
                        const size_t column = size_t(cx) + size_t(m_nx) * size_t(cz);
                        if (wet[column])
                        {
                            sum += heights[column];
                            ++count;
                        }
                    }
                }
                const size_t index = size_t(zi) * sideX + xi;
                const float x = s.origin[0] + h * xi;
                const float z = s.origin[2] + h * zi;
                if (count > 0)
                {
                    cornerY[index] = sum / float(count);
                    cornerWet[index] = 1;
                    ++m_wetVertices;
                }
                else
                {
                    // Dry corner: hide it under the bed (or the grid floor without one).
                    const float bed = solver.SampleSurfaceHeight(x, z);
                    cornerY[index] = (std::max)(bed, s.origin[1]) - 0.2f;
                }
            }
        }

        for (uint32_t zi = 0; zi < sideZ; ++zi)
        {
            for (uint32_t xi = 0; xi < sideX; ++xi)
            {
                const size_t index = size_t(zi) * sideX + xi;
                const uint32_t xl = xi > 0 ? xi - 1 : xi;
                const uint32_t xr = xi + 1 < sideX ? xi + 1 : xi;
                const uint32_t zl = zi > 0 ? zi - 1 : zi;
                const uint32_t zr = zi + 1 < sideZ ? zi + 1 : zi;
                const float dydx = (cornerY[size_t(zi) * sideX + xr] - cornerY[size_t(zi) * sideX + xl]) /
                                   (h * float(xr - xl));
                const float dydz = (cornerY[size_t(zr) * sideX + xi] - cornerY[size_t(zl) * sideX + xi]) /
                                   (h * float(zr - zl));
                float n[3] = { -dydx, 1.0f, -dydz };
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

                Vertex& v = m_mesh.vertices[index];
                v.position[0] = s.origin[0] + h * xi;
                v.position[1] = cornerY[index];
                v.position[2] = s.origin[2] + h * zi;
                for (int c = 0; c < 3; ++c)
                {
                    v.normal[c] = n[c] / len;
                    v.color[c] = m_waterColor[c];
                }
                v.color[3] = 1.0f;
                v.uv[0] = float(xi) / float(m_nx);
                v.uv[1] = float(zi) / float(m_nz);
            }
        }
        WriteSkirts(solver);
    }

    void FlipHeightFieldMesh::WriteSkirts(const FlipWaterSolver& solver)
    {
        const FlipWaterSettings& s = solver.Settings();
        uint32_t next = m_surfaceVertexCount;
        for (const SkirtEdge& edge : m_skirts)
        {
            const uint32_t count = static_cast<uint32_t>(edge.corners.size());
            for (uint32_t i = 0; i < count; ++i)
            {
                const Vertex& surface = m_mesh.vertices[edge.corners[i]];
                const float x = surface.position[0];
                const float z = surface.position[2];
                const float bed = (std::max)(solver.BedHeight(x, z), s.origin[1]);
                // A dry corner already sits below the bed; collapse its wall instead of drawing it upside down.
                const float top = (std::max)(surface.position[1], bed);
                Vertex& t = m_mesh.vertices[next + 2 * i];
                Vertex& b = m_mesh.vertices[next + 2 * i + 1];
                t = surface;
                t.position[1] = top;
                b = t;
                b.position[1] = bed;
                for (int c = 0; c < 3; ++c)
                {
                    t.normal[c] = edge.normal[c];
                    b.normal[c] = edge.normal[c];
                    // Deeper water reads darker through the side.
                    b.color[c] = m_waterColor[c] * 0.45f;
                }
            }
            next += 2 * count;
        }
    }
}
