#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    // Dense scalar field built by splatting particles, the first half of particle surfacing
    // (Zhu & Bridson 2005, "Animating Sand as a Fluid", sec. 6). Each particle adds
    //   w(d) = weight * k(d / radius),  k(s) = max(0, 1 - s^2)^3
    // to the density of every cell centre within `radius`, and the same w times its attribute to
    // a second accumulator, so Attribute() is the w-weighted mean (e.g. a flame's age). The kernel
    // is the one the paper uses for its weighted-average particle centre; summing it instead of
    // averaging gives a metaball-style density (Blinn 1982) whose level set closes over neighbours.
    // Cell (x, y, z) has its sample at origin + (x + 0.5, y + 0.5, z + 0.5) * cellSize.
    struct SplatGridDesc
    {
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float cellSize = 1.0f;
        uint32_t dims[3] = { 0, 0, 0 };
    };

    class ParticleSplatGrid
    {
    public:
        // Sizes the grid to cover the particles' AABB grown by maxRadius + 2 cells on every side, with
        // cells of `cellSize`, each axis capped at maxDim cells (the cell size grows to fit if needed, the
        // same on every axis). Clears both accumulators. count 0 gives dims 0 and an empty field.
        // positions: stride floats per particle, xyz first.
        void Fit(const float* positions, uint32_t count, uint32_t stride, float maxRadius, float cellSize, uint32_t maxDim);
        // Uses `desc` as is and clears both accumulators.
        void Reset(const SplatGridDesc& desc);

        // Adds one particle (see the class comment). Cells outside the grid are skipped.
        void Splat(const float position[3], float radius, float weight, float attribute);
        // Splats `count` particles; radii/weights/attributes may be null (then radius = defaultRadius,
        // weight = 1, attribute = 0). ParallelFor over z slabs, each slab looping over every particle
        // that overlaps it, so the result does not depend on the thread count (float sums run in particle order).
        void SplatAll(const float* positions, uint32_t count, uint32_t stride, const float* radii, float defaultRadius,
                      const float* weights, const float* attributes);
        // Anisotropic splat (Yu & Turk 2013 eq. 7-8): particle i adds weights[i] * k(|G_i (x - c_i)|) to the
        // density, with G_i the 9 row-major floats at matrices + 9 i and extents + 3 i the half-extents of
        // its ellipsoid along x, y, z (both from ComputeAnisotropicKernels). Attribute 0, weights may be null.
        // Same z-slab ParallelFor and particle-order sums as SplatAll.
        void SplatAllAnisotropic(const float* centres, uint32_t count, const float* matrices, const float* extents,
                                 const float* weights);

        const SplatGridDesc& Desc() const { return m_desc; }
        uint64_t CellCount() const { return uint64_t(m_desc.dims[0]) * m_desc.dims[1] * m_desc.dims[2]; }
        size_t Index(uint32_t x, uint32_t y, uint32_t z) const
        {
            return (size_t(z) * m_desc.dims[1] + y) * m_desc.dims[0] + x;
        }
        float Density(uint32_t x, uint32_t y, uint32_t z) const { return m_density[Index(x, y, z)]; }
        // Weighted mean attribute, 0 where the density is 0.
        float Attribute(uint32_t x, uint32_t y, uint32_t z) const;
        const std::vector<float>& DensityData() const { return m_density; }

    private:
        // Splat restricted to cells with z in [zBegin, zEnd); Splat is SplatSlab(..., 0, dims[2]).
        void SplatSlab(const float position[3], float radius, float weight, float attribute, uint32_t zBegin, uint32_t zEnd);
        void SplatAnisotropicSlab(const float centre[3], const float matrix[9], const float extent[3], float weight,
                                  uint32_t zBegin, uint32_t zEnd);

        SplatGridDesc m_desc;
        std::vector<float> m_density;
        std::vector<float> m_weightedAttribute;
    };
}

namespace SasamiEngine { using namespace Render; }
