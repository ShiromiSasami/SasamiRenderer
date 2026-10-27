#include "Renderer/Fluid/PbfSolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        constexpr float PI = 3.14159265358979f;

        float Poly6Coefficient(float h)
        {
            const float h3 = h * h * h;
            return 315.0f / (64.0f * PI * h3 * h3 * h3);
        }

        // Spiky gradient = coefficient * (h - r)^2 * rHat with a negative coefficient (paper eq. 21 kernel).
        float SpikyCoefficient(float h)
        {
            const float h3 = h * h * h;
            return -45.0f / (PI * h3 * h3);
        }

        void SpikyGradient(float dx, float dy, float dz, float r2, float h, float coefficient, float (&out)[3])
        {
            out[0] = out[1] = out[2] = 0.0f;
            if (r2 <= 1e-18f) {
                return;
            }
            const float r = std::sqrt(r2);
            const float scale = coefficient * (h - r) * (h - r) / r;
            out[0] = dx * scale;
            out[1] = dy * scale;
            out[2] = dz * scale;
        }
    }

    float Poly6Kernel(float r2, float h)
    {
        const float h2 = h * h;
        if (r2 >= h2) {
            return 0.0f;
        }
        const float d = h2 - r2;
        return Poly6Coefficient(h) * d * d * d;
    }

    void FindNeighborsBruteForce(const std::vector<float>& positions, float h, std::vector<std::vector<uint32_t>>& out)
    {
        const uint32_t count = static_cast<uint32_t>(positions.size() / 3);
        const float h2 = h * h;
        out.assign(count, {});
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t j = 0; j < count; ++j) {
                if (i == j) {
                    continue;
                }
                const float dx = positions[i * 3] - positions[j * 3];
                const float dy = positions[i * 3 + 1] - positions[j * 3 + 1];
                const float dz = positions[i * 3 + 2] - positions[j * 3 + 2];
                if (dx * dx + dy * dy + dz * dz < h2) {
                    out[i].push_back(j);
                }
            }
        }
    }

    void PbfSolver::Reset(const PbfParams& params)
    {
        m_params = params;
        m_params.particleMass = ComputeParticleMass(m_params);
        m_stats = {};
        m_positions.clear();
        m_velocities.clear();
        m_neighbors.clear();
        for (int axis = 0; axis < 3; ++axis) {
            const float extent = std::max(m_params.boxMax[axis] - m_params.boxMin[axis], 0.0f);
            m_gridDim[axis] = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(extent / m_params.smoothingRadius)));
        }
    }

    void PbfSolver::SpawnBlock(const float (&min)[3], const float (&max)[3])
    {
        const float spacing = 2.0f * m_params.particleRadius;
        int32_t counts[3];
        for (int axis = 0; axis < 3; ++axis) {
            counts[axis] = std::max(0, static_cast<int32_t>(std::floor((max[axis] - min[axis]) / spacing + 1e-3f)));
        }
        for (int32_t z = 0; z < counts[2]; ++z) {
            for (int32_t y = 0; y < counts[1]; ++y) {
                for (int32_t x = 0; x < counts[0]; ++x) {
                    m_positions.push_back(min[0] + spacing * (static_cast<float>(x) + 0.5f));
                    m_positions.push_back(min[1] + spacing * (static_cast<float>(y) + 0.5f));
                    m_positions.push_back(min[2] + spacing * (static_cast<float>(z) + 0.5f));
                    m_velocities.insert(m_velocities.end(), 3, 0.0f);
                }
            }
        }
    }

    void PbfSolver::BuildNeighbors(const std::vector<float>& positions)
    {
        const uint32_t count = static_cast<uint32_t>(positions.size() / 3);
        const float h = m_params.smoothingRadius;
        const float h2 = h * h;
        const int32_t dimX = static_cast<int32_t>(m_gridDim[0]);
        const int32_t dimY = static_cast<int32_t>(m_gridDim[1]);
        const int32_t dimZ = static_cast<int32_t>(m_gridDim[2]);
        const uint32_t cellTotal = m_gridDim[0] * m_gridDim[1] * m_gridDim[2];

        auto cellCoord = [&](float p, int axis) {
            const int32_t c = static_cast<int32_t>(std::floor((p - m_params.boxMin[axis]) / h));
            return std::clamp(c, 0, static_cast<int32_t>(m_gridDim[axis]) - 1);
        };

        // Counting sort of particle ids by cell; ids stay in index order inside a cell.
        m_cellOf.resize(count);
        m_cellStart.assign(cellTotal + 1, 0);
        for (uint32_t i = 0; i < count; ++i) {
            const int32_t cx = cellCoord(positions[i * 3], 0);
            const int32_t cy = cellCoord(positions[i * 3 + 1], 1);
            const int32_t cz = cellCoord(positions[i * 3 + 2], 2);
            const uint32_t cell = static_cast<uint32_t>((cz * dimY + cy) * dimX + cx);
            m_cellOf[i] = cell;
            ++m_cellStart[cell + 1];
        }
        for (uint32_t c = 0; c < cellTotal; ++c) {
            m_cellStart[c + 1] += m_cellStart[c];
        }
        m_cellFill.assign(m_cellStart.begin(), m_cellStart.end() - 1);
        m_sortedIds.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            m_sortedIds[m_cellFill[m_cellOf[i]]++] = i;
        }

        if (m_neighbors.size() != count) {
            m_neighbors.resize(count);
        }
        for (uint32_t i = 0; i < count; ++i) {
            std::vector<uint32_t>& list = m_neighbors[i];
            list.clear();
            const float px = positions[i * 3];
            const float py = positions[i * 3 + 1];
            const float pz = positions[i * 3 + 2];
            const int32_t cx = cellCoord(px, 0);
            const int32_t cy = cellCoord(py, 1);
            const int32_t cz = cellCoord(pz, 2);
            for (int32_t oz = std::max(cz - 1, 0); oz <= std::min(cz + 1, dimZ - 1); ++oz) {
                for (int32_t oy = std::max(cy - 1, 0); oy <= std::min(cy + 1, dimY - 1); ++oy) {
                    for (int32_t ox = std::max(cx - 1, 0); ox <= std::min(cx + 1, dimX - 1); ++ox) {
                        const uint32_t cell = static_cast<uint32_t>((oz * dimY + oy) * dimX + ox);
                        for (uint32_t s = m_cellStart[cell]; s < m_cellStart[cell + 1]; ++s) {
                            const uint32_t j = m_sortedIds[s];
                            if (j == i) {
                                continue;
                            }
                            const float dx = px - positions[j * 3];
                            const float dy = py - positions[j * 3 + 1];
                            const float dz = pz - positions[j * 3 + 2];
                            if (dx * dx + dy * dy + dz * dz < h2) {
                                list.push_back(j);
                            }
                        }
                    }
                }
            }
        }
    }

    void PbfSolver::Step(float dt)
    {
        const auto startTime = std::chrono::steady_clock::now();
        m_stats = {};
        const uint32_t count = ParticleCount();
        if (count == 0 || !(dt > 0.0f)) {
            return;
        }

        const PbfParams& p = m_params;
        const float h = p.smoothingRadius;
        const float h2 = h * h;
        const float invRho0 = 1.0f / p.restDensity;
        const float massOverRho0 = p.particleMass * invRho0;
        const float spikyCoefficient = SpikyCoefficient(h);
        const float poly6AtDq = Poly6Kernel((p.scorrDeltaQ * h) * (p.scorrDeltaQ * h), h);
        const float poly6AtZero = Poly6Kernel(0.0f, h);

        float lo[3];
        float hi[3];
        for (int axis = 0; axis < 3; ++axis) {
            lo[axis] = p.boxMin[axis] + p.particleRadius;
            hi[axis] = std::max(p.boxMax[axis] - p.particleRadius, lo[axis]);
        }

        m_predicted.resize(count * 3);
        m_delta.resize(count * 3);
        m_nextVelocities.resize(count * 3);
        m_lambda.assign(count, 0.0f);
        m_density.assign(count, p.restDensity);

        // Apply forces and predict positions.
        for (uint32_t i = 0; i < count; ++i) {
            m_velocities[i * 3 + 1] += p.gravityY * dt;
            for (int axis = 0; axis < 3; ++axis) {
                const float predicted = m_positions[i * 3 + axis] + dt * m_velocities[i * 3 + axis];
                m_predicted[i * 3 + axis] = std::clamp(predicted, lo[axis], hi[axis]);
            }
        }

        BuildNeighbors(m_predicted);

        for (uint32_t iteration = 0; iteration < p.solverIterations; ++iteration) {
            // Density constraint and lambda (eq. 8-11).
            for (uint32_t i = 0; i < count; ++i) {
                const float px = m_predicted[i * 3];
                const float py = m_predicted[i * 3 + 1];
                const float pz = m_predicted[i * 3 + 2];
                float density = poly6AtZero;
                float gradI[3] = { 0.0f, 0.0f, 0.0f };
                float sumGradSq = 0.0f;
                for (const uint32_t j : m_neighbors[i]) {
                    const float dx = px - m_predicted[j * 3];
                    const float dy = py - m_predicted[j * 3 + 1];
                    const float dz = pz - m_predicted[j * 3 + 2];
                    const float r2 = dx * dx + dy * dy + dz * dz;
                    if (r2 >= h2) {
                        continue;
                    }
                    density += Poly6Kernel(r2, h);
                    float grad[3];
                    SpikyGradient(dx, dy, dz, r2, h, spikyCoefficient, grad);
                    // grad wrt p_j is -(m / rho0) * gradW; wrt p_i it accumulates +(m / rho0) * gradW.
                    const float gx = massOverRho0 * grad[0];
                    const float gy = massOverRho0 * grad[1];
                    const float gz = massOverRho0 * grad[2];
                    sumGradSq += gx * gx + gy * gy + gz * gz;
                    gradI[0] += gx;
                    gradI[1] += gy;
                    gradI[2] += gz;
                }
                sumGradSq += gradI[0] * gradI[0] + gradI[1] * gradI[1] + gradI[2] * gradI[2];
                density *= p.particleMass;
                m_density[i] = density;
                const float constraint = density * invRho0 - 1.0f;
                m_lambda[i] = -constraint / (sumGradSq + p.relaxationEpsilon);
            }

            // Position delta with artificial pressure (eq. 12-14), computed for all particles before applying.
            for (uint32_t i = 0; i < count; ++i) {
                const float px = m_predicted[i * 3];
                const float py = m_predicted[i * 3 + 1];
                const float pz = m_predicted[i * 3 + 2];
                float sum[3] = { 0.0f, 0.0f, 0.0f };
                for (const uint32_t j : m_neighbors[i]) {
                    const float dx = px - m_predicted[j * 3];
                    const float dy = py - m_predicted[j * 3 + 1];
                    const float dz = pz - m_predicted[j * 3 + 2];
                    const float r2 = dx * dx + dy * dy + dz * dz;
                    if (r2 >= h2) {
                        continue;
                    }
                    float scorr = 0.0f;
                    if (poly6AtDq > 0.0f) {
                        scorr = -p.scorrK * std::pow(Poly6Kernel(r2, h) / poly6AtDq, p.scorrN);
                    }
                    float grad[3];
                    SpikyGradient(dx, dy, dz, r2, h, spikyCoefficient, grad);
                    const float weight = m_lambda[i] + m_lambda[j] + scorr;
                    sum[0] += weight * grad[0];
                    sum[1] += weight * grad[1];
                    sum[2] += weight * grad[2];
                }
                m_delta[i * 3] = massOverRho0 * sum[0];
                m_delta[i * 3 + 1] = massOverRho0 * sum[1];
                m_delta[i * 3 + 2] = massOverRho0 * sum[2];
            }

            for (uint32_t i = 0; i < count; ++i) {
                for (int axis = 0; axis < 3; ++axis) {
                    m_predicted[i * 3 + axis] = std::clamp(m_predicted[i * 3 + axis] + m_delta[i * 3 + axis], lo[axis], hi[axis]);
                }
            }
        }

        // Velocity update from the position change, then XSPH viscosity (eq. 17).
        const float invDt = 1.0f / dt;
        for (uint32_t i = 0; i < count * 3; ++i) {
            m_nextVelocities[i] = (m_predicted[i] - m_positions[i]) * invDt;
        }
        std::vector<float>& viscous = m_delta;
        for (uint32_t i = 0; i < count; ++i) {
            const float px = m_predicted[i * 3];
            const float py = m_predicted[i * 3 + 1];
            const float pz = m_predicted[i * 3 + 2];
            float sum[3] = { 0.0f, 0.0f, 0.0f };
            for (const uint32_t j : m_neighbors[i]) {
                const float dx = px - m_predicted[j * 3];
                const float dy = py - m_predicted[j * 3 + 1];
                const float dz = pz - m_predicted[j * 3 + 2];
                const float r2 = dx * dx + dy * dy + dz * dz;
                if (r2 >= h2) {
                    continue;
                }
                const float weight = Poly6Kernel(r2, h) * (p.particleMass / std::max(m_density[j], 1e-6f));
                for (int axis = 0; axis < 3; ++axis) {
                    sum[axis] += (m_nextVelocities[j * 3 + axis] - m_nextVelocities[i * 3 + axis]) * weight;
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                viscous[i * 3 + axis] = m_nextVelocities[i * 3 + axis] + p.xsphC * sum[axis];
            }
        }
        m_velocities = viscous;
        m_positions = m_predicted;

        double densitySum = 0.0;
        float densityMax = 0.0f;
        uint32_t neighborMax = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const float ratio = m_density[i] * invRho0;
            densitySum += ratio;
            densityMax = std::max(densityMax, ratio);
            neighborMax = std::max(neighborMax, static_cast<uint32_t>(m_neighbors[i].size()));
        }
        m_stats.meanDensityRatio = static_cast<float>(densitySum / count);
        m_stats.maxDensityRatio = densityMax;
        m_stats.maxNeighbors = neighborMax;
        m_stats.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
    }
}
