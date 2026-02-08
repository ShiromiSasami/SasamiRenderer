#pragma once
#include <vector>
#include <initializer_list>

#include "Renderer/Vertex.h"

namespace SasamiRenderer
{
    // Data-only mesh. Holds primitives (indices) and vertices.
    struct Mesh
    {
        // Default: unit quad on XY plane with +Z normal
        Mesh()
        {
            vertices = {
                { { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } },
                { {  1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
                { {  1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f } },
                { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f } },
            };
            indices = { 0, 1, 2, 0, 2, 3 };
        }

        // Construct from initializer lists (indexed)
        Mesh(std::initializer_list<Vertex> verts,
             std::initializer_list<uint32_t> inds)
        {
            vertices.assign(verts.begin(), verts.end());
            indices.assign(inds.begin(), inds.end());
        }

        // Construct from initializer list (non-indexed)
        explicit Mesh(std::initializer_list<Vertex> verts)
        {
            vertices.assign(verts.begin(), verts.end());
        }

        // Construct from C-arrays (indexed)
        template <size_t NV, size_t NI>
        Mesh(const Vertex (&verts)[NV], const uint32_t (&inds)[NI])
        {
            vertices.assign(&verts[0], &verts[0] + NV);
            indices.assign(&inds[0], &inds[0] + NI);
        }

        // Construct from C-array (non-indexed)
        template <size_t NV>
        explicit Mesh(const Vertex (&verts)[NV])
        {
            vertices.assign(&verts[0], &verts[0] + NV);
        }

        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices; // triangle list, multiple of 3

        bool HasIndices() const { return !indices.empty(); }
        uint32_t VertexCount() const { return static_cast<uint32_t>(vertices.size()); }
        uint32_t IndexCount()  const { return static_cast<uint32_t>(indices.size()); }
    };
}
