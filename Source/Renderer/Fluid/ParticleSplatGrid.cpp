#include "Renderer/Fluid/ParticleSplatGrid.h"

#include "Foundation/Jobs/ParallelFor.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        constexpr uint32_t SLAB_DEPTH = 4;
    }

    void ParticleSplatGrid::Fit(const float* positions, uint32_t count, uint32_t stride, float maxRadius, float cellSize, uint32_t maxDim)
    {
        if (count == 0 || cellSize <= 0.0f || maxDim == 0)
        {
            m_desc = SplatGridDesc();
            m_density.clear();
            m_weightedAttribute.clear();
            return;
        }

        float boxMin[3] = { positions[0], positions[1], positions[2] };
        float boxMax[3] = { positions[0], positions[1], positions[2] };
        for (uint32_t i = 1; i < count; ++i)
        {
            const float* p = positions + size_t(i) * stride;
            for (int a = 0; a < 3; ++a)
            {
                boxMin[a] = (std::min)(boxMin[a], p[a]);
                boxMax[a] = (std::max)(boxMax[a], p[a]);
            }
        }

        const float pad = maxRadius + 2.0f * cellSize;
        float extent[3];
        float maxExtent = 0.0f;
        for (int a = 0; a < 3; ++a)
        {
            extent[a] = (boxMax[a] - boxMin[a]) + 2.0f * pad;
            maxExtent = (std::max)(maxExtent, extent[a]);
        }

        float cs = cellSize;
        for (int a = 0; a < 3; ++a)
        {
            if (std::ceil(extent[a] / cs) > float(maxDim))
            {
                cs = maxExtent / float(maxDim);
                break;
            }
        }

        SplatGridDesc desc;
        desc.cellSize = cs;
        for (int a = 0; a < 3; ++a)
        {
            desc.origin[a] = boxMin[a] - pad;
            const float cells = std::ceil(extent[a] / cs);
            desc.dims[a] = (std::min)((std::max)(1u, uint32_t(cells)), maxDim);
        }
        Reset(desc);
    }

    void ParticleSplatGrid::Reset(const SplatGridDesc& desc)
    {
        m_desc = desc;
        m_density.assign(size_t(CellCount()), 0.0f);
        m_weightedAttribute.assign(size_t(CellCount()), 0.0f);
    }

    void ParticleSplatGrid::Splat(const float position[3], float radius, float weight, float attribute)
    {
        SplatSlab(position, radius, weight, attribute, 0, m_desc.dims[2]);
    }

    void ParticleSplatGrid::SplatSlab(const float position[3], float radius, float weight, float attribute, uint32_t zBegin, uint32_t zEnd)
    {
        if (radius <= 0.0f || weight == 0.0f)
        {
            return;
        }

        const float cs = m_desc.cellSize;
        uint32_t lo[3];
        uint32_t hi[3];
        for (int a = 0; a < 3; ++a)
        {
            if (m_desc.dims[a] == 0)
            {
                return;
            }
            const float last = float(m_desc.dims[a] - 1);
            float loF = std::ceil((position[a] - radius - m_desc.origin[a]) / cs - 0.5f);
            float hiF = std::floor((position[a] + radius - m_desc.origin[a]) / cs - 0.5f);
            loF = (std::max)(loF, 0.0f);
            hiF = (std::min)(hiF, last);
            if (a == 2)
            {
                loF = (std::max)(loF, float(zBegin));
                hiF = (std::min)(hiF, float(zEnd) - 1.0f);
            }
            if (loF > hiF)
            {
                return;
            }
            lo[a] = uint32_t(loF);
            hi[a] = uint32_t(hiF);
        }

        const float invR2 = 1.0f / (radius * radius);
        for (uint32_t z = lo[2]; z <= hi[2]; ++z)
        {
            const float dz = m_desc.origin[2] + (float(z) + 0.5f) * cs - position[2];
            for (uint32_t y = lo[1]; y <= hi[1]; ++y)
            {
                const float dy = m_desc.origin[1] + (float(y) + 0.5f) * cs - position[1];
                for (uint32_t x = lo[0]; x <= hi[0]; ++x)
                {
                    const float dx = m_desc.origin[0] + (float(x) + 0.5f) * cs - position[0];
                    const float s2 = (dx * dx + dy * dy + dz * dz) * invR2;
                    if (s2 < 1.0f)
                    {
                        const float t = 1.0f - s2;
                        const float w = weight * (t * t * t);
                        const size_t index = Index(x, y, z);
                        m_density[index] += w;
                        m_weightedAttribute[index] += w * attribute;
                    }
                }
            }
        }
    }

    void ParticleSplatGrid::SplatAll(const float* positions, uint32_t count, uint32_t stride, const float* radii, float defaultRadius,
                                     const float* weights, const float* attributes)
    {
        if (count == 0 || m_desc.dims[0] == 0 || m_desc.dims[1] == 0 || m_desc.dims[2] == 0)
        {
            return;
        }

        const uint32_t depth = m_desc.dims[2];
        const uint32_t numSlabs = (depth + SLAB_DEPTH - 1) / SLAB_DEPTH;
        Foundation::ParallelFor(numSlabs, [&](uint32_t s)
        {
            const uint32_t zBegin = s * SLAB_DEPTH;
            const uint32_t zEnd = (std::min)(zBegin + SLAB_DEPTH, depth);
            const float slabMinZ = m_desc.origin[2] + float(zBegin) * m_desc.cellSize;
            const float slabMaxZ = m_desc.origin[2] + float(zEnd) * m_desc.cellSize;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float* p = positions + size_t(i) * stride;
                const float radius = radii ? radii[i] : defaultRadius;
                if (p[2] + radius < slabMinZ || p[2] - radius > slabMaxZ)
                {
                    continue;
                }
                const float weight = weights ? weights[i] : 1.0f;
                const float attribute = attributes ? attributes[i] : 0.0f;
                SplatSlab(p, radius, weight, attribute, zBegin, zEnd);
            }
        });
    }

    void ParticleSplatGrid::SplatAnisotropicSlab(const float centre[3], const float matrix[9], const float extent[3],
                                                 float weight, uint32_t zBegin, uint32_t zEnd)
    {
        if (weight == 0.0f)
        {
            return;
        }

        const float cs = m_desc.cellSize;
        uint32_t lo[3];
        uint32_t hi[3];
        for (int a = 0; a < 3; ++a)
        {
            if (m_desc.dims[a] == 0 || !(extent[a] > 0.0f))
            {
                return;
            }
            const float last = float(m_desc.dims[a] - 1);
            float loF = std::ceil((centre[a] - extent[a] - m_desc.origin[a]) / cs - 0.5f);
            float hiF = std::floor((centre[a] + extent[a] - m_desc.origin[a]) / cs - 0.5f);
            loF = (std::max)(loF, 0.0f);
            hiF = (std::min)(hiF, last);
            if (a == 2)
            {
                loF = (std::max)(loF, float(zBegin));
                hiF = (std::min)(hiF, float(zEnd) - 1.0f);
            }
            if (loF > hiF)
            {
                return;
            }
            lo[a] = uint32_t(loF);
            hi[a] = uint32_t(hiF);
        }

        for (uint32_t z = lo[2]; z <= hi[2]; ++z)
        {
            const float dz = m_desc.origin[2] + (float(z) + 0.5f) * cs - centre[2];
            for (uint32_t y = lo[1]; y <= hi[1]; ++y)
            {
                const float dy = m_desc.origin[1] + (float(y) + 0.5f) * cs - centre[1];
                for (uint32_t x = lo[0]; x <= hi[0]; ++x)
                {
                    const float dx = m_desc.origin[0] + (float(x) + 0.5f) * cs - centre[0];
                    const float gx = matrix[0] * dx + matrix[1] * dy + matrix[2] * dz;
                    const float gy = matrix[3] * dx + matrix[4] * dy + matrix[5] * dz;
                    const float gz = matrix[6] * dx + matrix[7] * dy + matrix[8] * dz;
                    const float s2 = gx * gx + gy * gy + gz * gz;
                    if (s2 < 1.0f)
                    {
                        const float t = 1.0f - s2;
                        m_density[Index(x, y, z)] += weight * (t * t * t);
                    }
                }
            }
        }
    }

    void ParticleSplatGrid::SplatAllAnisotropic(const float* centres, uint32_t count, const float* matrices,
                                                const float* extents, const float* weights)
    {
        if (count == 0 || m_desc.dims[0] == 0 || m_desc.dims[1] == 0 || m_desc.dims[2] == 0)
        {
            return;
        }

        const uint32_t depth = m_desc.dims[2];
        const uint32_t numSlabs = (depth + SLAB_DEPTH - 1) / SLAB_DEPTH;
        Foundation::ParallelFor(numSlabs, [&](uint32_t s)
        {
            const uint32_t zBegin = s * SLAB_DEPTH;
            const uint32_t zEnd = (std::min)(zBegin + SLAB_DEPTH, depth);
            const float slabMinZ = m_desc.origin[2] + float(zBegin) * m_desc.cellSize;
            const float slabMaxZ = m_desc.origin[2] + float(zEnd) * m_desc.cellSize;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float* c = centres + size_t(i) * 3;
                const float* e = extents + size_t(i) * 3;
                if (c[2] + e[2] < slabMinZ || c[2] - e[2] > slabMaxZ)
                {
                    continue;
                }
                SplatAnisotropicSlab(c, matrices + size_t(i) * 9, e, weights ? weights[i] : 1.0f, zBegin, zEnd);
            }
        });
    }

    float ParticleSplatGrid::Attribute(uint32_t x, uint32_t y, uint32_t z) const
    {
        const size_t index = Index(x, y, z);
        const float density = m_density[index];
        return density > 0.0f ? m_weightedAttribute[index] / density : 0.0f;
    }
}
