#include "Renderer/Fluid/IsoSurfaceDynamicMesh.h"

#include <algorithm>

namespace SasamiEngine::Render::Fluid
{
    void IsoSurfaceDynamicMesh::Configure(uint32_t maxTriangles)
    {
        m_capacity = maxTriangles;
        const size_t slotVertices = size_t(maxTriangles) * 3;
        m_mesh.vertices.assign(slotVertices + BOUNDS_VERTEX_COUNT, Vertex{});
        m_mesh.indices.resize(slotVertices);
        for (size_t i = 0; i < slotVertices; ++i)
        {
            m_mesh.indices[i] = static_cast<uint32_t>(i);
        }
        for (Vertex& v : m_mesh.vertices)
        {
            v.normal[1] = 1.0f;
            v.color[3] = 1.0f;
        }
        m_written = 0;
        m_dirtyEnd = maxTriangles;
        m_dropped = 0;
    }

    uint32_t IsoSurfaceDynamicMesh::Update(const IsoSurfaceMesh& iso, IsoSurfaceVertexColor color,
                                           const float boundsMin[3], const float boundsMax[3])
    {
        const uint32_t triangles = iso.TriangleCount();
        const uint32_t written = (std::min)(triangles, m_capacity);
        const uint32_t sourceVertices = iso.VertexCount();
        m_colors.resize(size_t(sourceVertices) * 4);
        for (uint32_t i = 0; i < sourceVertices; ++i)
        {
            color(iso.attributes[i], &m_colors[size_t(i) * 4]);
        }
        for (uint32_t t = 0; t < written; ++t)
        {
            for (uint32_t k = 0; k < 3; ++k)
            {
                const uint32_t src = iso.indices[size_t(t) * 3 + k];
                Vertex& v = m_mesh.vertices[size_t(t) * 3 + k];
                for (int c = 0; c < 3; ++c)
                {
                    v.position[c] = iso.positions[size_t(src) * 3 + c];
                    v.normal[c] = iso.normals[size_t(src) * 3 + c];
                }
                for (int c = 0; c < 4; ++c)
                {
                    v.color[c] = m_colors[size_t(src) * 4 + c];
                }
            }
        }
        for (uint32_t t = written; t < m_dirtyEnd; ++t)
        {
            for (uint32_t k = 0; k < 3; ++k)
            {
                Vertex& v = m_mesh.vertices[size_t(t) * 3 + k];
                for (int c = 0; c < 3; ++c)
                {
                    v.position[c] = boundsMin[c];
                }
            }
        }
        Vertex* anchor = &m_mesh.vertices[size_t(m_capacity) * 3];
        for (int c = 0; c < 3; ++c)
        {
            anchor[0].position[c] = boundsMin[c];
            anchor[1].position[c] = boundsMax[c];
        }
        m_lastDirtyVertices = size_t((std::max)(written, m_dirtyEnd)) * 3;
        m_written = written;
        m_dirtyEnd = written;
        m_dropped = triangles - written;
        return written;
    }
}
