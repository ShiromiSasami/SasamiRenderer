#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    class ParticleSplatGrid;
    struct SplatGridDesc;

    // Indexed triangle mesh of one level set. Vertices are shared between triangles (one vertex per
    // crossed grid edge), so a closed surface has every undirected edge used by exactly two triangles.
    // Triangles wind counter-clockwise seen from outside (low density side); normals point outward.
    struct IsoSurfaceMesh
    {
        std::vector<float> positions;   // 3 per vertex, world
        std::vector<float> normals;     // 3 per vertex, unit, -grad(density) normalised
        std::vector<float> attributes;  // 1 per vertex, ParticleSplatGrid::Attribute interpolated
        std::vector<uint32_t> indices;  // 3 per triangle

        uint32_t VertexCount() const { return static_cast<uint32_t>(attributes.size()); }
        uint32_t TriangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }
        void Clear()
        {
            positions.clear();
            normals.clear();
            attributes.clear();
            indices.clear();
        }
    };

    // Polygonises the `iso` level of the grid's density with marching tetrahedra: every cell between
    // eight samples is cut into six tetrahedra around its main diagonal (Kuhn / Freudenthal split, the
    // same in every cell, so faces match and the mesh is watertight), and each tetrahedron yields 0-2
    // triangles. This is the table-free relative of marching cubes (Lorensen & Cline 1987): no
    // ambiguous cases and no 256-entry table to get wrong, at roughly twice the triangle count.
    // Samples outside the grid count as density 0, so a surface touching the border is still closed.
    // Vertices sit on cell edges by linear interpolation of density; normals come from the central-
    // difference density gradient interpolated the same way. Deterministic: z slabs of cells run in parallel
    // and are joined in a fixed order, giving the same mesh (vertices, indices, order) as one serial pass.
    void ExtractIsoSurface(const ParticleSplatGrid& grid, float iso, IsoSurfaceMesh& outMesh);
    // The same on a raw field laid out like ParticleSplatGrid (x fastest, sample at origin + (i + 0.5) *
    // cellSize). Samples outside the grid read `outsideValue`, which must be below `iso` so the surface
    // closes at the border. Attributes are all 0. Used for fields that are not particle splats (the voxel SDF).
    void ExtractIsoSurface(const SplatGridDesc& desc, const float* density, float iso, float outsideValue,
                           IsoSurfaceMesh& outMesh);

    // Volume enclosed and total area of a closed mesh (divergence theorem / triangle areas).
    float IsoSurfaceVolume(const IsoSurfaceMesh& mesh);
    float IsoSurfaceArea(const IsoSurfaceMesh& mesh);
    // Undirected edges not shared by exactly two triangles (0 for a closed, manifold mesh).
    uint32_t CountOpenEdges(const IsoSurfaceMesh& mesh);
}

namespace SasamiEngine { using namespace Render; }
