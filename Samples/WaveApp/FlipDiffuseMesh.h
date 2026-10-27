#pragma once

#include "FlipDiffuseParticles.h"
#include "FlipWaterSolver.h"
#include "Renderer/Structures/Mesh.h"

#include <cstdint>

namespace SasamiEngine::Sample::Wave
{
    // Foam c: draws the diffuse particles of FlipDiffuseParticles as small octahedra (6 vertices,
    // 8 triangles, normals from the centre) in one static mesh with a fixed topology, so Update()
    // only rewrites vertices. Above kMaxDrawn particles every n-th one is drawn; unused slots
    // collapse to one point under the grid floor. Spray and foam are white, bubbles pale blue.
    // The water is opaque, so bubbles show only where they are above the height-field surface.
    class FlipDiffuseMesh
    {
    public:
        static constexpr uint32_t kMaxDrawn = 16384;

        void Configure();
        void Update(const FlipWaterSolver& solver, const FlipDiffuseParticles& diffuse);

        const Mesh& GetMesh() const { return m_mesh; }
        uint32_t Drawn() const { return m_drawn; }
        // Drawn particles whose centre is above the drawn height field (the opaque water hides the rest).
        uint32_t Visible() const { return m_visible; }
        // Multiplies every radius (wave.diffuse size=).
        void SetSizeScale(float scale) { m_sizeScale = scale; }
        float SizeScale() const { return m_sizeScale; }

    private:
        Mesh m_mesh;
        uint32_t m_drawn = 0;
        uint32_t m_visible = 0;
        float m_sizeScale = 1.5f; // radii below are scaled by this; 1.5 picked by eye (foam8)
    };
}
