#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Velocity on a MAC (staggered) grid of dimX x dimY x dimZ cells (Harlow & Welch 1965; Bridson &
    // Mueller-Fischer 2007 course notes, ch. 2). Cell (x,y,z) covers voxel space [x, x+1] x ... and has
    // its centre at x+0.5 (the SignedDistanceGrid convention).
    //   u: x-faces, (dimX+1) * dimY * dimZ, face (i,j,k) sits at voxel point (i, j+0.5, k+0.5)
    //   v: y-faces, dimX * (dimY+1) * dimZ, face (i,j,k) sits at (i+0.5, j, k+0.5)
    //   w: z-faces, dimX * dimY * (dimZ+1), face (i,j,k) sits at (i+0.5, j+0.5, k)
    // Linear order is x fastest, then y, then z, over each component's own dims. Units: m/s.
    class StaggeredVelocityGrid
    {
    public:
        // Any zero dimension makes the grid empty (all dims 0). Values start at zero.
        void Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ);
        // Sets every u face to velocity[0], every v face to velocity[1], every w face to velocity[2].
        void Fill(const float velocity[3]);

        uint32_t DimX() const { return m_dimX; }
        uint32_t DimY() const { return m_dimY; }
        uint32_t DimZ() const { return m_dimZ; }

        // No range checks; i <= dimX for U, j <= dimY for V, k <= dimZ for W.
        float& U(uint32_t i, uint32_t j, uint32_t k);
        float& V(uint32_t i, uint32_t j, uint32_t k);
        float& W(uint32_t i, uint32_t j, uint32_t k);
        float U(uint32_t i, uint32_t j, uint32_t k) const;
        float V(uint32_t i, uint32_t j, uint32_t k) const;
        float W(uint32_t i, uint32_t j, uint32_t k) const;

        // Net outflow of cell (x,y,z), in m/s (face area 1 voxel^2, divided by the cell volume 1 voxel^3):
        // U(x+1,y,z) - U(x,y,z) + V(x,y+1,z) - V(x,y,z) + W(x,y,z+1) - W(x,y,z).
        float Divergence(uint32_t x, uint32_t y, uint32_t z) const;

        // Trilinear interpolation of each component on its own face lattice at a voxel-space point,
        // with the point clamped to that lattice (so outside the grid it returns the nearest boundary
        // value). Zero for an empty grid.
        void Sample(float vx, float vy, float vz, float outVelocity[3]) const;

        // Raw face arrays, x fastest: U is (dimX+1)*dimY*dimZ, V is dimX*(dimY+1)*dimZ, W is dimX*dimY*(dimZ+1).
        const std::vector<float>& UValues() const { return m_u; }
        const std::vector<float>& VValues() const { return m_v; }
        const std::vector<float>& WValues() const { return m_w; }

    private:
        uint32_t m_dimX = 0;
        uint32_t m_dimY = 0;
        uint32_t m_dimZ = 0;
        std::vector<float> m_u;
        std::vector<float> m_v;
        std::vector<float> m_w;
    };
}

namespace SasamiEngine { using namespace Render; }
