#pragma once

#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Structures/Mesh.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    // Colour of one iso-surface vertex from its interpolated attribute, linear RGBA (multiplies the albedo).
    using IsoSurfaceVertexColor = void (*)(float attribute, float outRgba[4]);

    // A renderer Mesh with a fixed topology that holds a changing iso-surface, so a caller can push it every
    // frame with Renderer::UpdateStaticMeshDynamicVertices (which only rewrites vertices of the same count).
    // Triangles are unshared: slot t owns vertices 3t..3t+2 and the index buffer is 0..3*capacity-1.
    // Triangles beyond the capacity are dropped (counted in Dropped()); unused slots collapse to one point
    // (zero area, nothing rasterised). Two trailing vertices that no index uses sit on the bounds box, so
    // the bounds the submitter computes once at upload cover every surface inside that box.
    // Unsharing costs 3 vertices per triangle instead of ~0.5 (the extractor shares them), but keeps the
    // index buffer constant, which the vertex-only update needs.
    class IsoSurfaceDynamicMesh
    {
    public:
        // Allocates the slots; every slot starts collapsed at the origin until the first Update.
        void Configure(uint32_t maxTriangles);
        // Copies `iso` into the slots (positions, normals, colour(attribute), uv 0) and collapses the slots
        // the previous Update used beyond the new count onto boundsMin. Colours are evaluated once per
        // extractor vertex, not per triangle corner. Returns the triangles written.
        uint32_t Update(const IsoSurfaceMesh& iso, IsoSurfaceVertexColor color,
                        const float boundsMin[3], const float boundsMax[3]);

        const Mesh& GetMesh() const { return m_mesh; }
        uint32_t Capacity() const { return m_capacity; }
        uint32_t Written() const { return m_written; }
        uint32_t Dropped() const { return m_dropped; }
        // Slot vertices the last Update changed are [0, DirtyVertexCount()); the two bounds vertices after the
        // slots (index BoundsVertexOffset()) are rewritten every Update too.
        size_t DirtyVertexCount() const { return m_lastDirtyVertices; }
        size_t BoundsVertexOffset() const { return size_t(m_capacity) * 3; }
        static constexpr size_t BOUNDS_VERTEX_COUNT = 2;

    private:
        Mesh m_mesh;
        uint32_t m_capacity = 0;
        uint32_t m_written = 0;
        uint32_t m_dirtyEnd = 0;    // slots [m_written, m_dirtyEnd) may still hold an old triangle
        uint32_t m_dropped = 0;
        size_t m_lastDirtyVertices = 0;
        std::vector<float> m_colors;    // scratch, 4 per extractor vertex
    };
}


namespace SasamiEngine { using namespace Render; }
