#include "FlipDiffuseParticles.h"

#include "FlipWaterSolver.h"
#include "Foundation/Jobs/ParallelFor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr float PI = 3.14159265358979f;
        constexpr uint32_t SURFACE_CHUNK = 512;
        constexpr float SPHERE_VOLUME_IN_CELLS = 4.0f / 3.0f * PI;   // radius h sphere / h^3 cell
        constexpr float NORMAL_DOT_VELOCITY_MIN = 0.6f;               // delta_vn, eq.7

        uint32_t NextRandom(uint32_t& state)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }

        float Random01(uint32_t& state)
        {
            return static_cast<float>(NextRandom(state) >> 8) * (1.0f / 16777216.0f);
        }

        // Eq.1.
        float ClampPotential(float value, float tauMin, float tauMax)
        {
            if (tauMax <= tauMin)
            {
                return value >= tauMax ? 1.0f : 0.0f;
            }
            return ((std::min)(value, tauMax) - (std::min)(value, tauMin)) / (tauMax - tauMin);
        }

        // Eq.3.
        float Weight(float distance, float h)
        {
            return distance <= h ? 1.0f - distance / h : 0.0f;
        }

        // Extra trapped air where water runs into a rock (not in Ihmsen et al. 2012, which only sees
        // fluid-fluid velocity differences; our own analogue for a fluid-solid impact): within h of
        // the rock, the speed toward it, max(0, -v.n), weighted by 1 - d/h. d and n are approximate:
        // d = (|(p - c)/r| - 1) * min(r), n = normalize((p - c)/r^2), the ellipsoid's gradient.
        float RockImpact(const FlipWaterSettings& s, const float p[3], const float v[3], float h)
        {
            float best = 0.0f;
            for (const FlipRock& rock : s.rocks)
            {
                float u[3];
                float n[3];
                float len2 = 0.0f;
                float nLen2 = 0.0f;
                for (int a = 0; a < 3; ++a)
                {
                    u[a] = (p[a] - rock.center[a]) / rock.radius[a];
                    n[a] = u[a] / rock.radius[a];
                    len2 += u[a] * u[a];
                    nLen2 += n[a] * n[a];
                }
                if (nLen2 < 1e-12f)
                {
                    continue;
                }
                const float rMin = (std::min)({ rock.radius[0], rock.radius[1], rock.radius[2] });
                const float d = (std::sqrt(len2) - 1.0f) * rMin;
                if (d >= h)
                {
                    continue;
                }
                const float invN = 1.0f / std::sqrt(nLen2);
                const float vn = (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) * invN;
                const float w = (std::min)(1.0f - (std::max)(d, 0.0f) / h, 1.0f);
                best = (std::max)(best, (std::max)(-vn, 0.0f) * w);
            }
            return best;
        }

        int CellOf(float x, float origin, float h, uint32_t n)
        {
            const int c = static_cast<int>(std::floor((x - origin) / h));
            return (std::clamp)(c, 0, static_cast<int>(n) - 1);
        }

        bool IsAirCell(const FlipCellGrid& cells, int x, int y, int z)
        {
            if (y >= static_cast<int>(cells.ny))
            {
                return true;
            }
            if (x < 0 || y < 0 || z < 0 || x >= static_cast<int>(cells.nx) || z >= static_cast<int>(cells.nz))
            {
                return false;
            }
            return cells.At(x, y, z) == FlipCell::Air;
        }

        bool NextToAir(const FlipCellGrid& cells, int x, int y, int z)
        {
            return IsAirCell(cells, x - 1, y, z) || IsAirCell(cells, x + 1, y, z) || IsAirCell(cells, x, y - 1, z) ||
                   IsAirCell(cells, x, y + 1, z) || IsAirCell(cells, x, y, z - 1) || IsAirCell(cells, x, y, z + 1);
        }

        // Two unit vectors orthogonal to the unit vector n.
        void OrthonormalBasis(const float n[3], float e1[3], float e2[3])
        {
            const float a[3] = { std::fabs(n[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
            e1[0] = a[1] * n[2] - a[2] * n[1];
            e1[1] = a[2] * n[0] - a[0] * n[2];
            e1[2] = a[0] * n[1] - a[1] * n[0];
            const float len = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
            for (int i = 0; i < 3; ++i)
            {
                e1[i] /= len;
            }
            e2[0] = n[1] * e1[2] - n[2] * e1[1];
            e2[1] = n[2] * e1[0] - n[0] * e1[2];
            e2[2] = n[0] * e1[1] - n[1] * e1[0];
        }

        template <typename Fn>
        void ForEachChunk(bool parallel, uint32_t count, Fn&& fn)
        {
            if (!parallel || count <= SURFACE_CHUNK)
            {
                fn(0u, count);
                return;
            }
            const uint32_t chunks = (count + SURFACE_CHUNK - 1) / SURFACE_CHUNK;
            Foundation::ParallelFor(chunks, [&](uint32_t chunk) {
                const uint32_t begin = chunk * SURFACE_CHUNK;
                fn(begin, (std::min)(count, begin + SURFACE_CHUNK));
            });
        }
    }

    void FlipDiffuseParticles::Reset()
    {
        m_x.clear();
        m_v.clear();
        m_life.clear();
        m_kind.clear();
        m_stats = {};
        m_rng = m_settings.seed ? m_settings.seed : 1u;
    }

    void FlipDiffuseParticles::Step(const FlipWaterSolver& water, float dt)
    {
        const Clock::time_point start = Clock::now();
        if (!m_seeded)
        {
            m_rng = m_settings.seed ? m_settings.seed : 1u;
            m_seeded = true;
        }
        m_stats.spawned = 0;
        m_stats.removed = 0;
        if (!m_settings.enabled || dt <= 0.0f || water.ParticleCount() == 0)
        {
            m_stats.stepMs = 0.0f;
            return;
        }
        BuildFluidHash(water);
        BuildDensity(water);
        Advance(water, dt);
        Emit(water, dt);

        m_stats.spray = 0;
        m_stats.foam = 0;
        m_stats.bubble = 0;
        for (uint8_t k : m_kind)
        {
            switch (static_cast<DiffuseKind>(k))
            {
            case DiffuseKind::Spray: ++m_stats.spray; break;
            case DiffuseKind::Foam: ++m_stats.foam; break;
            case DiffuseKind::Bubble: ++m_stats.bubble; break;
            }
        }
        m_stats.stepMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    }

    void FlipDiffuseParticles::BuildFluidHash(const FlipWaterSolver& water)
    {
        const FlipWaterSettings& s = water.Settings();
        const FlipCellGrid& cells = water.Cells();
        const std::vector<float>& px = water.ParticlePositions();
        const uint32_t count = water.ParticleCount();
        const size_t cellCount = cells.CellCount();

        std::vector<uint32_t> cellOf(count);
        m_cellStart.assign(cellCount + 1, 0);
        for (uint32_t p = 0; p < count; ++p)
        {
            const int i = CellOf(px[3 * p], s.origin[0], s.cellSize, cells.nx);
            const int j = CellOf(px[3 * p + 1], s.origin[1], s.cellSize, cells.ny);
            const int k = CellOf(px[3 * p + 2], s.origin[2], s.cellSize, cells.nz);
            cellOf[p] = static_cast<uint32_t>(cells.Index(i, j, k));
            ++m_cellStart[cellOf[p] + 1];
        }
        for (size_t c = 0; c < cellCount; ++c)
        {
            m_cellStart[c + 1] += m_cellStart[c];
        }
        std::vector<uint32_t> cursor(m_cellStart.begin(), m_cellStart.end() - 1);
        m_sorted.resize(count);
        for (uint32_t p = 0; p < count; ++p)
        {
            m_sorted[cursor[cellOf[p]]++] = p;
        }

        m_surface.assign(count, 0);
        for (uint32_t z = 0; z < cells.nz; ++z)
        {
            for (uint32_t y = 0; y < cells.ny; ++y)
            {
                for (uint32_t x = 0; x < cells.nx; ++x)
                {
                    const size_t c = cells.Index(x, y, z);
                    if (m_cellStart[c] == m_cellStart[c + 1] || !NextToAir(cells, x, y, z))
                    {
                        continue;
                    }
                    for (uint32_t n = m_cellStart[c]; n < m_cellStart[c + 1]; ++n)
                    {
                        m_surface[m_sorted[n]] = 1;
                    }
                }
            }
        }
    }

    void FlipDiffuseParticles::BuildDensity(const FlipWaterSolver& water)
    {
        const size_t cellCount = water.Cells().CellCount();
        m_density.resize(cellCount);
        for (size_t c = 0; c < cellCount; ++c)
        {
            m_density[c] = static_cast<float>(m_cellStart[c + 1] - m_cellStart[c]);
        }
    }

    float FlipDiffuseParticles::NeighbourCount(const FlipWaterSolver& water, const float x[3]) const
    {
        const FlipWaterSettings& s = water.Settings();
        const FlipCellGrid& cells = water.Cells();
        const int n[3] = { static_cast<int>(cells.nx), static_cast<int>(cells.ny), static_cast<int>(cells.nz) };
        int c0[3];
        float f[3];
        for (int a = 0; a < 3; ++a)
        {
            const float g = (x[a] - s.origin[a]) / s.cellSize - 0.5f;
            const float fl = std::floor(g);
            c0[a] = static_cast<int>(fl);
            f[a] = g - fl;
        }
        float sum = 0.0f;
        for (int dz = 0; dz < 2; ++dz)
        {
            for (int dy = 0; dy < 2; ++dy)
            {
                for (int dx = 0; dx < 2; ++dx)
                {
                    const int i = c0[0] + dx;
                    const int j = c0[1] + dy;
                    const int k = c0[2] + dz;
                    if (i < 0 || j < 0 || k < 0 || i >= n[0] || j >= n[1] || k >= n[2])
                    {
                        continue;
                    }
                    const float w = (dx ? f[0] : 1.0f - f[0]) * (dy ? f[1] : 1.0f - f[1]) * (dz ? f[2] : 1.0f - f[2]);
                    sum += w * m_density[cells.Index(i, j, k)];
                }
            }
        }
        return sum * SPHERE_VOLUME_IN_CELLS;
    }

    void FlipDiffuseParticles::Emit(const FlipWaterSolver& water, float dt)
    {
        const FlipWaterSettings& s = water.Settings();
        const FlipCellGrid& cells = water.Cells();
        const std::vector<float>& px = water.ParticlePositions();
        const std::vector<float>& pv = water.ParticleVelocities();
        const uint32_t count = water.ParticleCount();
        const float h = s.cellSize;

        // Candidates: particles next to Air, plus those striking a rock (below the surface too, so an
        // impact under the crest still entrains air; not in the paper).
        std::vector<uint32_t> surface;
        for (uint32_t p = 0; p < count; ++p)
        {
            if (m_surface[p] || (!s.rocks.empty() && RockImpact(s, &px[3 * p], &pv[3 * p], h) > 0.0f))
            {
                surface.push_back(p);
            }
        }
        const uint32_t surfaceCount = static_cast<uint32_t>(surface.size());
        m_stats.surfaceParticles = surfaceCount;
        m_normal.assign(size_t(count) * 3, 0.0f);

        // Visits every fluid particle j != p within h of particle p (27 cells).
        auto forNeighbours = [&](uint32_t p, auto&& fn) {
            const int ci = CellOf(px[3 * p], s.origin[0], h, cells.nx);
            const int cj = CellOf(px[3 * p + 1], s.origin[1], h, cells.ny);
            const int ck = CellOf(px[3 * p + 2], s.origin[2], h, cells.nz);
            for (int k = (std::max)(ck - 1, 0); k <= (std::min)(ck + 1, static_cast<int>(cells.nz) - 1); ++k)
            {
                for (int j = (std::max)(cj - 1, 0); j <= (std::min)(cj + 1, static_cast<int>(cells.ny) - 1); ++j)
                {
                    for (int i = (std::max)(ci - 1, 0); i <= (std::min)(ci + 1, static_cast<int>(cells.nx) - 1); ++i)
                    {
                        const size_t c = cells.Index(i, j, k);
                        for (uint32_t n = m_cellStart[c]; n < m_cellStart[c + 1]; ++n)
                        {
                            const uint32_t q = m_sorted[n];
                            if (q == p)
                            {
                                continue;
                            }
                            const float d[3] = { px[3 * p] - px[3 * q], px[3 * p + 1] - px[3 * q + 1],
                                                 px[3 * p + 2] - px[3 * q + 2] };
                            const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                            const float w = Weight(dist, h);
                            if (w > 0.0f && dist > 0.0f)
                            {
                                fn(q, d, dist, w);
                            }
                        }
                    }
                }
            }
        };

        // Pass 1: surface normals, n_i = normalize(sum_j (x_i - x_j) W).
        ForEachChunk(s.parallel, surfaceCount, [&](uint32_t begin, uint32_t end) {
            for (uint32_t idx = begin; idx < end; ++idx)
            {
                const uint32_t p = surface[idx];
                float n[3] = { 0.0f, 0.0f, 0.0f };
                forNeighbours(p, [&](uint32_t, const float d[3], float, float w) {
                    n[0] += d[0] * w;
                    n[1] += d[1] * w;
                    n[2] += d[2] * w;
                });
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len > 1e-12f)
                {
                    for (int a = 0; a < 3; ++a)
                    {
                        m_normal[3 * p + a] = n[a] / len;
                    }
                }
            }
        });

        // Pass 2: potentials. Curvature only sums over surface neighbours (deviation: interior
        // particles have no normal here).
        std::vector<float> trapped(surfaceCount, 0.0f);
        std::vector<float> crest(surfaceCount, 0.0f);
        std::vector<float> energy(surfaceCount, 0.0f);
        ForEachChunk(s.parallel, surfaceCount, [&](uint32_t begin, uint32_t end) {
            for (uint32_t idx = begin; idx < end; ++idx)
            {
                const uint32_t p = surface[idx];
                const float* vi = &pv[3 * p];
                const float* ni = &m_normal[3 * p];
                float vDiff = 0.0f;
                float kappa = 0.0f;
                forNeighbours(p, [&](uint32_t q, const float d[3], float dist, float w) {
                    const float* vj = &pv[3 * q];
                    const float vij[3] = { vi[0] - vj[0], vi[1] - vj[1], vi[2] - vj[2] };
                    const float vLen = std::sqrt(vij[0] * vij[0] + vij[1] * vij[1] + vij[2] * vij[2]);
                    if (vLen > 1e-12f)
                    {
                        const float cosine = (vij[0] * d[0] + vij[1] * d[1] + vij[2] * d[2]) / (vLen * dist);
                        vDiff += vLen * (1.0f - cosine) * w;   // eq.2
                    }
                    if (m_surface[q])
                    {
                        // eq.6: x^_ji . n^_i < 0 (neighbour behind the surface) counts, d = x_i - x_j.
                        const float behind = -(d[0] * ni[0] + d[1] * ni[1] + d[2] * ni[2]);
                        if (behind < 0.0f)
                        {
                            const float* nj = &m_normal[3 * q];
                            kappa += (1.0f - (ni[0] * nj[0] + ni[1] * nj[1] + ni[2] * nj[2])) * w;
                        }
                    }
                });
                const float speed2 = vi[0] * vi[0] + vi[1] * vi[1] + vi[2] * vi[2];
                const float speed = std::sqrt(speed2);
                float deltaVn = 0.0f;
                if (speed > 1e-12f)
                {
                    const float vn = (vi[0] * ni[0] + vi[1] * ni[1] + vi[2] * ni[2]) / speed;
                    deltaVn = vn >= NORMAL_DOT_VELOCITY_MIN ? 1.0f : 0.0f;   // eq.7
                }
                vDiff += RockImpact(s, &px[3 * p], vi, h) * m_settings.rockImpactScale;
                trapped[idx] = vDiff;
                crest[idx] = kappa * deltaVn;
                energy[idx] = 0.5f * speed2;
            }
        });

        // Serial emission keeps the random sequence independent of the thread count.
        const FlipDiffuseSettings& c = m_settings;
        const float rV = 0.5f * h / static_cast<float>((std::max)(s.particlesPerAxis, 1u));
        m_stats.maxTrappedAir = 0.0f;
        m_stats.maxCurvature = 0.0f;
        m_stats.maxEnergy = 0.0f;
        for (uint32_t idx = 0; idx < surfaceCount; ++idx)
        {
            m_stats.maxTrappedAir = (std::max)(m_stats.maxTrappedAir, trapped[idx]);
            m_stats.maxCurvature = (std::max)(m_stats.maxCurvature, crest[idx]);
            m_stats.maxEnergy = (std::max)(m_stats.maxEnergy, energy[idx]);
            const float iTa = ClampPotential(trapped[idx], c.trappedAirMin, c.trappedAirMax);
            const float iWc = ClampPotential(crest[idx], c.waveCrestMin, c.waveCrestMax);
            const float iK = ClampPotential(energy[idx], c.energyMin, c.energyMax);
            const float expected = iK * (c.trappedAirRate * iTa + c.waveCrestRate * iWc) * dt;   // eq.8
            if (expected <= 0.0f)
            {
                continue;
            }
            uint32_t nd = static_cast<uint32_t>(expected);
            if (Random01(m_rng) < expected - static_cast<float>(nd))
            {
                ++nd;
            }
            if (nd == 0)
            {
                continue;
            }

            const uint32_t p = surface[idx];
            const float* xf = &px[3 * p];
            const float* vf = &pv[3 * p];
            const float speed = std::sqrt(vf[0] * vf[0] + vf[1] * vf[1] + vf[2] * vf[2]);
            const float axis[3] = { vf[0] / speed, vf[1] / speed, vf[2] / speed };   // iK > 0 -> speed > 0
            float e1[3];
            float e2[3];
            OrthonormalBasis(axis, e1, e2);
            // Lifetime grows with the generation potential (deviation: the paper only says it is
            // set at generation; the mapping is ours).
            const float potential = (std::min)(1.0f, iK * 0.5f * (iTa + iWc));
            const float life = c.lifetimeMin + (c.lifetimeMax - c.lifetimeMin) * potential;
            for (uint32_t d = 0; d < nd; ++d)
            {
                if (Count() >= c.maxParticles)
                {
                    break;
                }
                const float r = rV * std::sqrt(Random01(m_rng));
                const float theta = 2.0f * PI * Random01(m_rng);
                const float hgt = Random01(m_rng);
                const float cr = r * std::cos(theta);
                const float sr = r * std::sin(theta);
                float x[3];
                float v[3];
                for (int a = 0; a < 3; ++a)
                {
                    const float radial = cr * e1[a] + sr * e2[a];
                    x[a] = xf[a] + radial + hgt * dt * vf[a];
                    v[a] = radial + vf[a];
                }
                water.ConfineToDomain(x, v);
                m_x.insert(m_x.end(), x, x + 3);
                m_v.insert(m_v.end(), v, v + 3);
                m_life.push_back(life);
                const float neighbours = NeighbourCount(water, x);
                m_kind.push_back(static_cast<uint8_t>(neighbours < c.sprayMaxNeighbours ? DiffuseKind::Spray
                                                      : neighbours > c.bubbleMinNeighbours ? DiffuseKind::Bubble
                                                                                           : DiffuseKind::Foam));
                ++m_stats.spawned;
            }
        }
    }

    void FlipDiffuseParticles::Advance(const FlipWaterSolver& water, float dt)
    {
        const FlipWaterSettings& s = water.Settings();
        const float invH = 1.0f / s.cellSize;
        const FlipDiffuseSettings& c = m_settings;
        const uint32_t count = Count();
        std::vector<uint8_t> dead(count, 0);
        ForEachChunk(s.parallel, count, [&](uint32_t begin, uint32_t end) {
            for (uint32_t p = begin; p < end; ++p)
            {
                float* x = &m_x[3 * p];
                float* v = &m_v[3 * p];
                const float neighbours = NeighbourCount(water, x);
                const DiffuseKind kind = neighbours < c.sprayMaxNeighbours  ? DiffuseKind::Spray
                                         : neighbours > c.bubbleMinNeighbours ? DiffuseKind::Bubble
                                                                              : DiffuseKind::Foam;
                m_kind[p] = static_cast<uint8_t>(kind);
                float vf[3];
                water.Velocity().Sample((x[0] - s.origin[0]) * invH, (x[1] - s.origin[1]) * invH,
                                        (x[2] - s.origin[2]) * invH, vf);
                if (kind == DiffuseKind::Spray)
                {
                    v[1] -= dt * s.gravity;   // no other external force
                }
                else if (kind == DiffuseKind::Foam)
                {
                    v[0] = vf[0];
                    v[1] = vf[1];
                    v[2] = vf[2];
                    m_life[p] -= dt;   // only foam dissolves
                    if (m_life[p] <= 0.0f)
                    {
                        dead[p] = 1;
                    }
                }
                else
                {
                    // v += dt (-k_b g + k_d (v_f - v) / dt), g = (0, -gravity, 0).
                    v[1] += dt * c.buoyancy * s.gravity;
                    for (int a = 0; a < 3; ++a)
                    {
                        v[a] += c.drag * (vf[a] - v[a]);
                    }
                }
                for (int a = 0; a < 3; ++a)
                {
                    x[a] += dt * v[a];
                }
                water.ConfineToDomain(x, v);
            }
        });

        uint32_t out = 0;
        for (uint32_t p = 0; p < count; ++p)
        {
            if (dead[p])
            {
                ++m_stats.removed;
                continue;
            }
            if (out != p)
            {
                for (int a = 0; a < 3; ++a)
                {
                    m_x[3 * out + a] = m_x[3 * p + a];
                    m_v[3 * out + a] = m_v[3 * p + a];
                }
                m_life[out] = m_life[p];
                m_kind[out] = m_kind[p];
            }
            ++out;
        }
        m_x.resize(size_t(out) * 3);
        m_v.resize(size_t(out) * 3);
        m_life.resize(out);
        m_kind.resize(out);
    }
}
