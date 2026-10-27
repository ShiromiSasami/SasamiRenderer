#include "Renderer/Fluid/IsoSurfaceRenderMesh.h"

namespace SasamiEngine::Render::Fluid
{
    Mesh BuildIsoSurfaceRenderMesh(const IsoSurfaceMesh& iso, const float rgba[4])
    {
        Mesh mesh;
        const size_t vertexCount = iso.positions.size() / 3;
        mesh.vertices.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            Vertex& v = mesh.vertices[i];
            for (int c = 0; c < 3; ++c)
            {
                v.position[c] = iso.positions[i * 3 + c];
                v.normal[c] = iso.normals[i * 3 + c];
            }
            for (int c = 0; c < 4; ++c)
            {
                v.color[c] = rgba[c];
            }
            v.uv[0] = 0.0f;
            v.uv[1] = 0.0f;
        }
        mesh.indices = iso.indices;
        return mesh;
    }
}
