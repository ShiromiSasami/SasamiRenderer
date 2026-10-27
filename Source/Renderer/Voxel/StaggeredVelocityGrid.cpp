#include "Renderer/Voxel/StaggeredVelocityGrid.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        // Trilinear sample of one component on a lattice of latticeDim points per axis; (gx,gy,gz) is the
        // point in that lattice's own coordinates and gets clamped to [0, latticeDim - 1].
        float SampleLattice(const std::vector<float>& values, const uint32_t latticeDim[3], float gx, float gy, float gz)
        {
            const float g[3] = { gx, gy, gz };
            uint32_t i0[3];
            uint32_t i1[3];
            float frac[3];
            for (int axis = 0; axis < 3; ++axis) {
                const float maxIndex = static_cast<float>(latticeDim[axis] - 1);
                const float c = std::min(std::max(g[axis], 0.0f), maxIndex);
                const float f = std::floor(c);
                i0[axis] = static_cast<uint32_t>(f);
                i1[axis] = std::min(i0[axis] + 1u, latticeDim[axis] - 1u);
                frac[axis] = c - f;
            }
            const auto at = [&](uint32_t i, uint32_t j, uint32_t k) {
                return values[i + latticeDim[0] * (j + static_cast<size_t>(latticeDim[1]) * k)];
            };
            const float x00 = at(i0[0], i0[1], i0[2]) + (at(i1[0], i0[1], i0[2]) - at(i0[0], i0[1], i0[2])) * frac[0];
            const float x10 = at(i0[0], i1[1], i0[2]) + (at(i1[0], i1[1], i0[2]) - at(i0[0], i1[1], i0[2])) * frac[0];
            const float x01 = at(i0[0], i0[1], i1[2]) + (at(i1[0], i0[1], i1[2]) - at(i0[0], i0[1], i1[2])) * frac[0];
            const float x11 = at(i0[0], i1[1], i1[2]) + (at(i1[0], i1[1], i1[2]) - at(i0[0], i1[1], i1[2])) * frac[0];
            const float y0 = x00 + (x10 - x00) * frac[1];
            const float y1 = x01 + (x11 - x01) * frac[1];
            return y0 + (y1 - y0) * frac[2];
        }
    }

    void StaggeredVelocityGrid::Reset(uint32_t dimX, uint32_t dimY, uint32_t dimZ)
    {
        if (dimX == 0 || dimY == 0 || dimZ == 0) {
            m_dimX = m_dimY = m_dimZ = 0;
            m_u.clear();
            m_v.clear();
            m_w.clear();
            return;
        }
        m_dimX = dimX;
        m_dimY = dimY;
        m_dimZ = dimZ;
        const size_t x = dimX;
        const size_t y = dimY;
        const size_t z = dimZ;
        m_u.assign((x + 1) * y * z, 0.0f);
        m_v.assign(x * (y + 1) * z, 0.0f);
        m_w.assign(x * y * (z + 1), 0.0f);
    }

    void StaggeredVelocityGrid::Fill(const float velocity[3])
    {
        std::fill(m_u.begin(), m_u.end(), velocity[0]);
        std::fill(m_v.begin(), m_v.end(), velocity[1]);
        std::fill(m_w.begin(), m_w.end(), velocity[2]);
    }

    float& StaggeredVelocityGrid::U(uint32_t i, uint32_t j, uint32_t k)
    {
        return m_u[i + static_cast<size_t>(m_dimX + 1) * (j + static_cast<size_t>(m_dimY) * k)];
    }

    float& StaggeredVelocityGrid::V(uint32_t i, uint32_t j, uint32_t k)
    {
        return m_v[i + static_cast<size_t>(m_dimX) * (j + static_cast<size_t>(m_dimY + 1) * k)];
    }

    float& StaggeredVelocityGrid::W(uint32_t i, uint32_t j, uint32_t k)
    {
        return m_w[i + static_cast<size_t>(m_dimX) * (j + static_cast<size_t>(m_dimY) * k)];
    }

    float StaggeredVelocityGrid::U(uint32_t i, uint32_t j, uint32_t k) const
    {
        return m_u[i + static_cast<size_t>(m_dimX + 1) * (j + static_cast<size_t>(m_dimY) * k)];
    }

    float StaggeredVelocityGrid::V(uint32_t i, uint32_t j, uint32_t k) const
    {
        return m_v[i + static_cast<size_t>(m_dimX) * (j + static_cast<size_t>(m_dimY + 1) * k)];
    }

    float StaggeredVelocityGrid::W(uint32_t i, uint32_t j, uint32_t k) const
    {
        return m_w[i + static_cast<size_t>(m_dimX) * (j + static_cast<size_t>(m_dimY) * k)];
    }

    float StaggeredVelocityGrid::Divergence(uint32_t x, uint32_t y, uint32_t z) const
    {
        return U(x + 1, y, z) - U(x, y, z) + V(x, y + 1, z) - V(x, y, z) + W(x, y, z + 1) - W(x, y, z);
    }

    void StaggeredVelocityGrid::Sample(float vx, float vy, float vz, float outVelocity[3]) const
    {
        if (m_dimX == 0) {
            outVelocity[0] = outVelocity[1] = outVelocity[2] = 0.0f;
            return;
        }
        const uint32_t uDim[3] = { m_dimX + 1, m_dimY, m_dimZ };
        const uint32_t vDim[3] = { m_dimX, m_dimY + 1, m_dimZ };
        const uint32_t wDim[3] = { m_dimX, m_dimY, m_dimZ + 1 };
        outVelocity[0] = SampleLattice(m_u, uDim, vx, vy - 0.5f, vz - 0.5f);
        outVelocity[1] = SampleLattice(m_v, vDim, vx - 0.5f, vy, vz - 0.5f);
        outVelocity[2] = SampleLattice(m_w, wDim, vx - 0.5f, vy - 0.5f, vz);
    }
}
