#pragma once

#include "FlipWaterSolver.h"
#include "Renderer/Structures/Mesh.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    // Water surface of the FLIP solver as a height field: one vertex per column corner, height = mean
    // of the wet columns around the corner. Corners with no wet column sink 0.2 m below the bed so the
    // beach mesh covers them. The four tank sides get a vertical skirt from the surface down to the bed
    // (or the grid floor), so the tank reads as a cross-section instead of exposing the dug-out bed
    // around it. Topology is fixed for a given grid, so Update() only rewrites vertices.
    class FlipHeightFieldMesh
    {
    public:
        void Configure(const FlipWaterSolver& solver, const float waterColor[3]);
        void Update(const FlipWaterSolver& solver);

        const Mesh& GetMesh() const { return m_mesh; }
        uint32_t WetVertices() const { return m_wetVertices; }

    private:
        // Corner indices of one tank side, ordered along d = up x outward normal, so the skirt quads
        // wind the same way as the surface.
        struct SkirtEdge
        {
            std::vector<uint32_t> corners;
            float normal[3];
        };

        void WriteSkirts(const FlipWaterSolver& solver);

        Mesh m_mesh;
        std::vector<SkirtEdge> m_skirts;
        uint32_t m_surfaceVertexCount = 0;
        uint32_t m_nx = 0;
        uint32_t m_nz = 0;
        float m_waterColor[3] = { 0.03f, 0.18f, 0.24f };
        uint32_t m_wetVertices = 0;
    };
}
