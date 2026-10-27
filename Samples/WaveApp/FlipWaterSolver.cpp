#include "FlipWaterSolver.h"

#include "Foundation/Jobs/ParallelFor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace SasamiEngine::Sample::Wave
{
    float RockTopHeight(const FlipRock& rock, float x, float z)
    {
        if (rock.radius[0] <= 0.0f || rock.radius[1] <= 0.0f || rock.radius[2] <= 0.0f)
        {
            return -std::numeric_limits<float>::infinity();
        }
        const float dx = (x - rock.center[0]) / rock.radius[0];
        const float dz = (z - rock.center[2]) / rock.radius[2];
        const float q = 1.0f - dx * dx - dz * dz;
        if (q <= 0.0f)
        {
            return -std::numeric_limits<float>::infinity();
        }
        return rock.center[1] + rock.radius[1] * std::sqrt(q);
    }

    namespace
    {
        using Clock = std::chrono::steady_clock;
        using Render::Voxel::StaggeredVelocityGrid;

        constexpr float PI = 3.14159265358979f;
        constexpr uint32_t PARTICLE_CHUNK = 4096;

        float ElapsedMs(Clock::time_point from, Clock::time_point to)
        {
            return std::chrono::duration<float, std::milli>(to - from).count();
        }

        float SeaBedAt(SeaBedHeightFunction seaBed, const FlipWaterSettings& s, float x, float z)
        {
            float bed = seaBed ? seaBed(x, z) : s.origin[1] - 1.0f;
            for (const FlipRock& rock : s.rocks)
            {
                bed = (std::max)(bed, RockTopHeight(rock, x, z));
            }
            return bed;
        }

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

        // keepBox == nullptr: keep particles below waterLevel; else keep those inside [boxMin, boxMax].
        void SeedParticles(const FlipWaterSettings& s, SeaBedHeightFunction seaBed,
                           const std::vector<uint8_t>& staticSolid, const float* boxMin, const float* boxMax,
                           std::vector<float>& px, std::vector<float>& pv)
        {
            px.clear();
            pv.clear();
            uint32_t state = s.seed * 2654435761u ^ 0x9E3779B9u;
            if (state == 0)
            {
                state = 1;
            }
            const uint32_t ppa = (std::max)(s.particlesPerAxis, 1u);
            const float h = s.cellSize;
            const float sub = h / static_cast<float>(ppa);
            for (uint32_t k = 0; k < s.nz; ++k)
            {
                for (uint32_t j = 0; j < s.ny; ++j)
                {
                    for (uint32_t i = 0; i < s.nx; ++i)
                    {
                        if (staticSolid[i + size_t(s.nx) * (j + size_t(s.ny) * k)])
                        {
                            continue;
                        }
                        for (uint32_t c = 0; c < ppa; ++c)
                        {
                            for (uint32_t b = 0; b < ppa; ++b)
                            {
                                for (uint32_t a = 0; a < ppa; ++a)
                                {
                                    const float jx = (Random01(state) - 0.5f) * 0.5f;
                                    const float jy = (Random01(state) - 0.5f) * 0.5f;
                                    const float jz = (Random01(state) - 0.5f) * 0.5f;
                                    const float x = s.origin[0] + h * i + sub * (a + 0.5f + jx);
                                    const float y = s.origin[1] + h * j + sub * (b + 0.5f + jy);
                                    const float z = s.origin[2] + h * k + sub * (c + 0.5f + jz);
                                    if (y <= SeaBedAt(seaBed, s, x, z))
                                    {
                                        continue;
                                    }
                                    if (boxMin)
                                    {
                                        if (x < boxMin[0] || x > boxMax[0] || y < boxMin[1] || y > boxMax[1] ||
                                            z < boxMin[2] || z > boxMax[2])
                                        {
                                            continue;
                                        }
                                    }
                                    else if (y >= s.waterLevel)
                                    {
                                        continue;
                                    }
                                    px.push_back(x);
                                    px.push_back(y);
                                    px.push_back(z);
                                    pv.push_back(0.0f);
                                    pv.push_back(0.0f);
                                    pv.push_back(0.0f);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Cells outside the grid are Solid except above the top, which is open Air.
        FlipCell KindAt(const FlipCellGrid& cells, int i, int j, int k)
        {
            if (i < 0 || i >= static_cast<int>(cells.nx) || k < 0 || k >= static_cast<int>(cells.nz) || j < 0)
            {
                return FlipCell::Solid;
            }
            if (j >= static_cast<int>(cells.ny))
            {
                return FlipCell::Air;
            }
            return cells.At(i, j, k);
        }

        // Face (i,j,k) of component comp lies between cell lo = face - unit axis and cell hi = face.
        FlipCell LowCell(const FlipCellGrid& cells, int comp, int i, int j, int k)
        {
            return KindAt(cells, i - (comp == 0), j - (comp == 1), k - (comp == 2));
        }

        bool IsSolidFace(const FlipCellGrid& cells, int comp, int i, int j, int k)
        {
            if (comp == 1 && j == static_cast<int>(cells.ny))
            {
                return false;
            }
            return LowCell(cells, comp, i, j, k) == FlipCell::Solid || KindAt(cells, i, j, k) == FlipCell::Solid;
        }

        bool HasFluidNeighbour(const FlipCellGrid& cells, int comp, int i, int j, int k)
        {
            return LowCell(cells, comp, i, j, k) == FlipCell::Fluid || KindAt(cells, i, j, k) == FlipCell::Fluid;
        }

        void FaceDims(const FlipCellGrid& cells, int comp, uint32_t out[3])
        {
            out[0] = cells.nx + (comp == 0 ? 1u : 0u);
            out[1] = cells.ny + (comp == 1 ? 1u : 0u);
            out[2] = cells.nz + (comp == 2 ? 1u : 0u);
        }

        float& FaceRef(StaggeredVelocityGrid& g, int comp, uint32_t i, uint32_t j, uint32_t k)
        {
            return comp == 0 ? g.U(i, j, k) : (comp == 1 ? g.V(i, j, k) : g.W(i, j, k));
        }

        int CellCoord(float world, float origin, float h, uint32_t n)
        {
            const int c = static_cast<int>(std::floor((world - origin) / h));
            return (std::max)(0, (std::min)(c, static_cast<int>(n) - 1));
        }

        template <typename Fn>
        void ForEachParticleChunk(bool parallel, uint32_t count, Fn&& fn)
        {
            if (!parallel || count <= PARTICLE_CHUNK)
            {
                fn(0u, count);
                return;
            }
            const uint32_t chunks = (count + PARTICLE_CHUNK - 1) / PARTICLE_CHUNK;
            Foundation::ParallelFor(chunks, [&](uint32_t chunk) {
                const uint32_t begin = chunk * PARTICLE_CHUNK;
                fn(begin, (std::min)(count, begin + PARTICLE_CHUNK));
            });
        }
    }

    float FlipWaterSolver::BedHeight(float x, float z) const
    {
        return SeaBedAt(m_seaBed, m_settings, x, z);
    }

    float FlipWaterSolver::SeaBed(float x, float z) const
    {
        return SeaBedAt(m_seaBed, m_settings, x, z);
    }

    bool FlipWaterSolver::ConfineToDomain(float position[3], float velocity[3]) const
    {
        const float h = m_settings.cellSize;
        const float eps = 1e-3f * h;
        const float extent[3] = { m_settings.nx * h, m_settings.ny * h, m_settings.nz * h };
        bool moved = false;
        for (int a = 0; a < 3; ++a)
        {
            const float lo = m_settings.origin[a] + eps;
            const float hi = m_settings.origin[a] + extent[a] - eps;
            if (position[a] < lo)
            {
                position[a] = lo;
                velocity[a] = (std::max)(velocity[a], 0.0f);
                moved = true;
            }
            else if (position[a] > hi)
            {
                position[a] = hi;
                velocity[a] = (std::min)(velocity[a], 0.0f);
                moved = true;
            }
        }
        const float bed = SeaBed(position[0], position[2]) + eps;
        if (position[1] < bed)
        {
            position[1] = bed;
            velocity[1] = (std::max)(velocity[1], 0.0f);
            moved = true;
        }
        return moved;
    }

    void FlipWaterSolver::Reset(const FlipWaterSettings& settings, SeaBedHeightFunction seaBed)
    {
        m_settings = settings;
        m_seaBed = seaBed;
        m_paddleDriveU = 0.0f;
        m_paddleDriveK = 0.0f;
        m_velocity.Reset(settings.nx, settings.ny, settings.nz);
        m_savedVelocity.Reset(settings.nx, settings.ny, settings.nz);

        m_cells.nx = settings.nx;
        m_cells.ny = settings.ny;
        m_cells.nz = settings.nz;
        m_cells.type.assign(m_cells.CellCount(), static_cast<uint8_t>(FlipCell::Air));
        m_staticSolid.assign(m_cells.CellCount(), 0);
        const float h = settings.cellSize;
        for (uint32_t k = 0; k < settings.nz; ++k)
        {
            for (uint32_t j = 0; j < settings.ny; ++j)
            {
                for (uint32_t i = 0; i < settings.nx; ++i)
                {
                    const float cx = settings.origin[0] + h * (i + 0.5f);
                    const float cy = settings.origin[1] + h * (j + 0.5f);
                    const float cz = settings.origin[2] + h * (k + 0.5f);
                    if (cy < SeaBed(cx, cz))
                    {
                        m_staticSolid[m_cells.Index(i, j, k)] = 1;
                    }
                }
            }
        }

        SeedParticles(m_settings, m_seaBed, m_staticSolid, nullptr, nullptr, m_px, m_pv);
        m_layout = TallCellLayout{};
        m_quadtree = QuadtreeColumnLayout{};
        m_stats = FlipWaterStats{};
        ClassifyCells();
        UpdateColumnHeights();
    }

    void FlipWaterSolver::ResetToBlock(const float boxMin[3], const float boxMax[3])
    {
        m_velocity.Reset(m_settings.nx, m_settings.ny, m_settings.nz);
        m_savedVelocity.Reset(m_settings.nx, m_settings.ny, m_settings.nz);
        SeedParticles(m_settings, m_seaBed, m_staticSolid, boxMin, boxMax, m_px, m_pv);
        m_layout = TallCellLayout{};
        m_quadtree = QuadtreeColumnLayout{};
        m_stats = FlipWaterStats{};
        ClassifyCells();
        UpdateColumnHeights();
    }

    void FlipWaterSolver::Step(float dt)
    {
        if (dt <= 0.0f)
        {
            return;
        }
        const auto start = Clock::now();

        float maxSpeed = 0.0f;
        for (size_t p = 0; p + 2 < m_pv.size(); p += 3)
        {
            const float sp = std::sqrt(m_pv[p] * m_pv[p] + m_pv[p + 1] * m_pv[p + 1] + m_pv[p + 2] * m_pv[p + 2]);
            maxSpeed = (std::max)(maxSpeed, sp);
        }
        const float speedEstimate = maxSpeed + m_settings.gravity * dt;
        const float cflDt = m_settings.maxCfl * m_settings.cellSize / (std::max)(speedEstimate, 1e-6f);
        const uint32_t wanted = static_cast<uint32_t>(std::ceil(dt / cflDt));
        const uint32_t substeps = (std::max)(1u, (std::min)(wanted, (std::max)(m_settings.maxSubsteps, 1u)));

        uint32_t iterations = 0;
        float residual = 0.0f;
        bool converged = true;
        float p2gMs = 0.0f;
        float solveMs = 0.0f;
        float g2pMs = 0.0f;
        const float subDt = dt / static_cast<float>(substeps);
        for (uint32_t s = 0; s < substeps; ++s)
        {
            Substep(subDt);
            iterations += m_stats.iterations;
            residual = (std::max)(residual, m_stats.residual);
            converged = converged && m_stats.converged;
            p2gMs += m_stats.p2gMs;
            solveMs += m_stats.solveMs;
            g2pMs += m_stats.g2pMs;
        }
        m_stats.iterations = iterations;
        m_stats.residual = residual;
        m_stats.converged = converged;
        m_stats.p2gMs = p2gMs;
        m_stats.solveMs = solveMs;
        m_stats.g2pMs = g2pMs;
        m_stats.substeps = substeps;

        UpdateColumnHeights();
        m_stats.stepMs = ElapsedMs(start, Clock::now());
    }

    void FlipWaterSolver::Substep(float dt)
    {
        const auto t0 = Clock::now();
        ClassifyCells();
        TransferToGrid();
        m_savedVelocity = m_velocity;

        const float gdt = m_settings.gravity * dt;
        for (uint32_t k = 0; k < m_settings.nz; ++k)
        {
            for (uint32_t j = 0; j <= m_settings.ny; ++j)
            {
                for (uint32_t i = 0; i < m_settings.nx; ++i)
                {
                    m_velocity.V(i, j, k) -= gdt;
                }
            }
        }
        ApplyBoundaryVelocities();
        const auto t1 = Clock::now();

        Project();
        const auto t2 = Clock::now();

        Extrapolate();
        ApplyBoundaryVelocities();
        const auto t3 = Clock::now();

        TransferToParticles();
        Advect(dt);
        const auto t4 = Clock::now();

        m_stats.p2gMs = ElapsedMs(t0, t1);
        m_stats.solveMs = ElapsedMs(t1, t2);
        m_stats.g2pMs = ElapsedMs(t3, t4);
        m_stats.time += dt;
    }

    void FlipWaterSolver::ClassifyCells()
    {
        const size_t count = m_cells.CellCount();
        uint32_t solid = 0;
        for (size_t c = 0; c < count; ++c)
        {
            const bool isSolid = m_staticSolid[c] != 0;
            m_cells.type[c] = static_cast<uint8_t>(isSolid ? FlipCell::Solid : FlipCell::Air);
            solid += isSolid ? 1u : 0u;
        }
        const float h = m_settings.cellSize;
        for (size_t p = 0; p + 2 < m_px.size(); p += 3)
        {
            const int i = CellCoord(m_px[p], m_settings.origin[0], h, m_cells.nx);
            const int j = CellCoord(m_px[p + 1], m_settings.origin[1], h, m_cells.ny);
            const int k = CellCoord(m_px[p + 2], m_settings.origin[2], h, m_cells.nz);
            uint8_t& type = m_cells.type[m_cells.Index(i, j, k)];
            if (type == static_cast<uint8_t>(FlipCell::Air))
            {
                type = static_cast<uint8_t>(FlipCell::Fluid);
            }
        }
        uint32_t fluid = 0;
        for (size_t c = 0; c < count; ++c)
        {
            fluid += (m_cells.type[c] == static_cast<uint8_t>(FlipCell::Fluid)) ? 1u : 0u;
        }
        m_stats.fluidCells = fluid;
        m_stats.solidCells = solid;
    }

    void FlipWaterSolver::TransferToGrid()
    {
        const float invH = 1.0f / m_settings.cellSize;
        const uint32_t count = ParticleCount();
        for (int comp = 0; comp < 3; ++comp)
        {
            uint32_t d[3];
            FaceDims(m_cells, comp, d);
            const size_t faces = size_t(d[0]) * d[1] * d[2];
            std::vector<float> sum(faces, 0.0f);
            std::vector<float> weight(faces, 0.0f);
            const float off[3] = { comp == 0 ? 0.0f : 0.5f, comp == 1 ? 0.0f : 0.5f, comp == 2 ? 0.0f : 0.5f };

            for (uint32_t p = 0; p < count; ++p)
            {
                uint32_t base[3];
                uint32_t next[3];
                float t[3];
                for (int a = 0; a < 3; ++a)
                {
                    float f = (m_px[3 * p + a] - m_settings.origin[a]) * invH - off[a];
                    f = (std::max)(0.0f, (std::min)(f, static_cast<float>(d[a] - 1)));
                    base[a] = (std::min)(static_cast<uint32_t>(f), d[a] - 1);
                    next[a] = (std::min)(base[a] + 1, d[a] - 1);
                    t[a] = f - static_cast<float>(base[a]);
                }
                const float v = m_pv[3 * p + comp];
                for (int corner = 0; corner < 8; ++corner)
                {
                    const int cx = corner & 1;
                    const int cy = (corner >> 1) & 1;
                    const int cz = (corner >> 2) & 1;
                    const float w = (cx ? t[0] : 1.0f - t[0]) * (cy ? t[1] : 1.0f - t[1]) * (cz ? t[2] : 1.0f - t[2]);
                    const uint32_t i = cx ? next[0] : base[0];
                    const uint32_t j = cy ? next[1] : base[1];
                    const uint32_t k = cz ? next[2] : base[2];
                    const size_t idx = i + size_t(d[0]) * (j + size_t(d[1]) * k);
                    sum[idx] += w * v;
                    weight[idx] += w;
                }
            }

            for (uint32_t k = 0; k < d[2]; ++k)
            {
                for (uint32_t j = 0; j < d[1]; ++j)
                {
                    for (uint32_t i = 0; i < d[0]; ++i)
                    {
                        const size_t idx = i + size_t(d[0]) * (j + size_t(d[1]) * k);
                        FaceRef(m_velocity, comp, i, j, k) = weight[idx] > 0.0f ? sum[idx] / weight[idx] : 0.0f;
                    }
                }
            }
        }
    }

    void FlipWaterSolver::SetPaddleDrive(float surfaceVelocity, float decayPerMeter)
    {
        m_paddleDriveU = surfaceVelocity;
        m_paddleDriveK = (std::max)(decayPerMeter, 0.0f);
    }

    void FlipWaterSolver::ApplyBoundaryVelocities()
    {
        float paddle = 0.0f;
        if (!m_settings.paddleExternal && m_settings.paddleAmplitude != 0.0f && m_settings.paddlePeriod > 0.0f)
        {
            paddle = m_settings.paddleAmplitude *
                     std::cos(2.0f * PI * static_cast<float>(m_stats.time) / m_settings.paddlePeriod);
        }
        for (int comp = 0; comp < 3; ++comp)
        {
            uint32_t d[3];
            FaceDims(m_cells, comp, d);
            for (uint32_t k = 0; k < d[2]; ++k)
            {
                for (uint32_t j = 0; j < d[1]; ++j)
                {
                    for (uint32_t i = 0; i < d[0]; ++i)
                    {
                        if (!IsSolidFace(m_cells, comp, i, j, k))
                        {
                            continue;
                        }
                        float value = 0.0f;
                        if (comp == 0 && i == 0 && KindAt(m_cells, 0, j, k) != FlipCell::Solid)
                        {
                            value = paddle;
                            if (m_settings.paddleExternal)
                            {
                                const float y = m_settings.origin[1] + (static_cast<float>(j) + 0.5f) * m_settings.cellSize;
                                const float below = (std::min)(y - m_settings.waterLevel, 0.0f);
                                value = m_paddleDriveU * std::exp(m_paddleDriveK * below);
                            }
                        }
                        FaceRef(m_velocity, comp, i, j, k) = value;
                    }
                }
            }
        }
    }

    void FlipWaterSolver::Project()
    {
        const uint32_t nx = m_cells.nx;
        const uint32_t ny = m_cells.ny;
        const uint32_t nz = m_cells.nz;
        m_divergence.assign(m_cells.CellCount(), 0.0f);
        for (uint32_t k = 0; k < nz; ++k)
        {
            for (uint32_t j = 0; j < ny; ++j)
            {
                for (uint32_t i = 0; i < nx; ++i)
                {
                    if (m_cells.At(i, j, k) == FlipCell::Fluid)
                    {
                        m_divergence[m_cells.Index(i, j, k)] = m_velocity.Divergence(i, j, k);
                    }
                }
            }
        }

        // Sizing reads this substep's velocity before projection (the paper uses the previous step's).
        m_quadtree = QuadtreeColumnLayout{};
        uint32_t marked = 0;
        if (m_settings.quadtree)
        {
            ColumnSizingStats sizingStats;
            const std::vector<uint8_t> desired = ComputeColumnDesiredLog2(m_cells, m_velocity, m_settings.sizing, &sizingStats);
            m_quadtree = BuildQuadtreeColumnLayout(nx, nz, desired, m_settings.sizing.maxLog2);
            marked = sizingStats.markedColumns;
        }

        PressureSolveStats solve;
        OpticalLayerStats opticalStats;
        if (m_settings.tallCells || m_settings.quadtree)
        {
            m_layout = TallCellLayout{};
            if (m_settings.tallCells)
            {
                std::vector<uint16_t> bands;
                if (m_settings.opticalLayers.enabled)
                    bands = ComputeOpticalLayerBands(m_cells, m_velocity, m_settings.opticalLayers, &opticalStats);
                m_layout = BuildTallCellLayout(m_cells, m_settings.bandCells, m_settings.minTallCells,
                                               bands.empty() ? nullptr : &bands);
                if (m_settings.quadtree)
                {
                    RestrictTallCellsToLeaves(m_layout, m_quadtree, m_settings.minTallCells);
                }
            }
            solve = SolveTallCellPressure(m_cells, m_layout, m_divergence, m_pressure, m_settings.pressure,
                                          m_settings.quadtree ? &m_quadtree : nullptr);
        }
        else
        {
            m_layout = TallCellLayout{};
            solve = SolveUniformPressure(m_cells, m_divergence, m_pressure, m_settings.pressure);
        }

        auto pressureAt = [&](int i, int j, int k) {
            if (i < 0 || i >= static_cast<int>(nx) || j < 0 || j >= static_cast<int>(ny) || k < 0 ||
                k >= static_cast<int>(nz))
            {
                return 0.0f;
            }
            return m_pressure[m_cells.Index(i, j, k)];
        };
        for (int comp = 0; comp < 3; ++comp)
        {
            uint32_t d[3];
            FaceDims(m_cells, comp, d);
            for (uint32_t k = 0; k < d[2]; ++k)
            {
                for (uint32_t j = 0; j < d[1]; ++j)
                {
                    for (uint32_t i = 0; i < d[0]; ++i)
                    {
                        if (IsSolidFace(m_cells, comp, i, j, k) || !HasFluidNeighbour(m_cells, comp, i, j, k))
                        {
                            continue;
                        }
                        const float pHigh = pressureAt(i, j, k);
                        const float pLow = pressureAt(i - (comp == 0), j - (comp == 1), k - (comp == 2));
                        FaceRef(m_velocity, comp, i, j, k) -= pHigh - pLow;
                    }
                }
            }
        }

        float maxDiv = 0.0f;
        float maxDivTall = 0.0f;
        float maxDivCoarse = 0.0f;
        for (uint32_t k = 0; k < nz; ++k)
        {
            for (uint32_t j = 0; j < ny; ++j)
            {
                for (uint32_t i = 0; i < nx; ++i)
                {
                    if (m_cells.At(i, j, k) != FlipCell::Fluid)
                    {
                        continue;
                    }
                    const float div = std::fabs(m_velocity.Divergence(i, j, k));
                    bool inTall = false;
                    if (!m_layout.columns.empty())
                    {
                        const TallCellColumn& col = m_layout.At(i, k);
                        inTall = col.top >= col.bottom + 2 && j >= col.bottom && j < col.top;
                    }
                    const bool inCoarse = !m_quadtree.leafOf.empty() &&
                                          m_quadtree.leaves[m_quadtree.leafOf[i + size_t(nx) * k]].level > 0;
                    float& target = inTall ? maxDivTall : (inCoarse ? maxDivCoarse : maxDiv);
                    target = (std::max)(target, div);
                }
            }
        }
        m_stats.maxDivergence = maxDiv;
        m_stats.maxDivergenceTall = maxDivTall;
        m_stats.maxDivergenceCoarse = maxDivCoarse;
        m_stats.quadLeaves = static_cast<uint32_t>(m_quadtree.leaves.size());
        m_stats.quadFineColumns = m_quadtree.leaves.empty() ? 0 : m_quadtree.leavesPerLevel[0];
        m_stats.quadMarked = marked;
        m_stats.opticalMinBand = opticalStats.minBand;
        m_stats.opticalMaxBand = opticalStats.maxBand;
        m_stats.opticalMeanBand = opticalStats.meanBand;
        m_stats.opticalMaxIndicator = opticalStats.maxIndicator;
        m_stats.unknowns = solve.unknowns;
        m_stats.iterations = solve.iterations;
        m_stats.residual = solve.residual;
        m_stats.converged = solve.converged;
        m_stats.tallColumns = m_layout.tallColumns;
        m_stats.mergedCells = m_layout.mergedCells;
        m_stats.solveMs = solve.buildMs + solve.solveMs;
    }

    void FlipWaterSolver::Extrapolate()
    {
        for (int comp = 0; comp < 3; ++comp)
        {
            uint32_t d[3];
            FaceDims(m_cells, comp, d);
            const size_t faces = size_t(d[0]) * d[1] * d[2];
            std::vector<uint8_t> valid(faces, 0);
            for (uint32_t k = 0; k < d[2]; ++k)
            {
                for (uint32_t j = 0; j < d[1]; ++j)
                {
                    for (uint32_t i = 0; i < d[0]; ++i)
                    {
                        const size_t idx = i + size_t(d[0]) * (j + size_t(d[1]) * k);
                        valid[idx] = (IsSolidFace(m_cells, comp, i, j, k) ||
                                      HasFluidNeighbour(m_cells, comp, i, j, k)) ? 1 : 0;
                    }
                }
            }

            struct Update
            {
                uint32_t i, j, k;
                float value;
            };
            std::vector<Update> updates;
            for (int sweep = 0; sweep < 2; ++sweep)
            {
                updates.clear();
                for (uint32_t k = 0; k < d[2]; ++k)
                {
                    for (uint32_t j = 0; j < d[1]; ++j)
                    {
                        for (uint32_t i = 0; i < d[0]; ++i)
                        {
                            if (valid[i + size_t(d[0]) * (j + size_t(d[1]) * k)])
                            {
                                continue;
                            }
                            float sum = 0.0f;
                            int n = 0;
                            for (int axis = 0; axis < 3; ++axis)
                            {
                                for (int sign = -1; sign <= 1; sign += 2)
                                {
                                    const int ni = static_cast<int>(i) + (axis == 0 ? sign : 0);
                                    const int nj = static_cast<int>(j) + (axis == 1 ? sign : 0);
                                    const int nk = static_cast<int>(k) + (axis == 2 ? sign : 0);
                                    if (ni < 0 || nj < 0 || nk < 0 || ni >= static_cast<int>(d[0]) ||
                                        nj >= static_cast<int>(d[1]) || nk >= static_cast<int>(d[2]))
                                    {
                                        continue;
                                    }
                                    if (valid[ni + size_t(d[0]) * (nj + size_t(d[1]) * nk)])
                                    {
                                        sum += FaceRef(m_velocity, comp, ni, nj, nk);
                                        ++n;
                                    }
                                }
                            }
                            if (n > 0)
                            {
                                updates.push_back({ i, j, k, sum / static_cast<float>(n) });
                            }
                        }
                    }
                }
                for (const Update& u : updates)
                {
                    FaceRef(m_velocity, comp, u.i, u.j, u.k) = u.value;
                    valid[u.i + size_t(d[0]) * (u.j + size_t(d[1]) * u.k)] = 1;
                }
            }
        }
    }

    void FlipWaterSolver::TransferToParticles()
    {
        const float invH = 1.0f / m_settings.cellSize;
        const float flip = m_settings.flipRatio;
        ForEachParticleChunk(m_settings.parallel, ParticleCount(), [&](uint32_t begin, uint32_t end) {
            for (uint32_t p = begin; p < end; ++p)
            {
                const float gx = (m_px[3 * p] - m_settings.origin[0]) * invH;
                const float gy = (m_px[3 * p + 1] - m_settings.origin[1]) * invH;
                const float gz = (m_px[3 * p + 2] - m_settings.origin[2]) * invH;
                float vNew[3];
                float vOld[3];
                m_velocity.Sample(gx, gy, gz, vNew);
                m_savedVelocity.Sample(gx, gy, gz, vOld);
                for (int a = 0; a < 3; ++a)
                {
                    const float flipV = m_pv[3 * p + a] + (vNew[a] - vOld[a]);
                    m_pv[3 * p + a] = flip * flipV + (1.0f - flip) * vNew[a];
                }
            }
        });
    }

    void FlipWaterSolver::Advect(float dt)
    {
        const float h = m_settings.cellSize;
        const float invH = 1.0f / h;
        ForEachParticleChunk(m_settings.parallel, ParticleCount(), [&](uint32_t begin, uint32_t end) {
            for (uint32_t p = begin; p < end; ++p)
            {
                float x[3] = { m_px[3 * p], m_px[3 * p + 1], m_px[3 * p + 2] };
                float v1[3];
                float v2[3];
                m_velocity.Sample((x[0] - m_settings.origin[0]) * invH, (x[1] - m_settings.origin[1]) * invH,
                                  (x[2] - m_settings.origin[2]) * invH, v1);
                const float mid[3] = { x[0] + 0.5f * dt * v1[0], x[1] + 0.5f * dt * v1[1], x[2] + 0.5f * dt * v1[2] };
                m_velocity.Sample((mid[0] - m_settings.origin[0]) * invH, (mid[1] - m_settings.origin[1]) * invH,
                                  (mid[2] - m_settings.origin[2]) * invH, v2);
                for (int a = 0; a < 3; ++a)
                {
                    x[a] += dt * v2[a];
                }
                ConfineToDomain(x, &m_pv[3 * p]);
                m_px[3 * p] = x[0];
                m_px[3 * p + 1] = x[1];
                m_px[3 * p + 2] = x[2];
            }
        });
    }

    void FlipWaterSolver::UpdateColumnHeights()
    {
        const uint32_t nx = m_settings.nx;
        const uint32_t nz = m_settings.nz;
        const float h = m_settings.cellSize;
        const size_t columns = size_t(nx) * nz;
        const float lowest = std::numeric_limits<float>::lowest();
        std::vector<float> maxY(columns, lowest);
        float maxSpeed = 0.0f;
        for (uint32_t p = 0; p < ParticleCount(); ++p)
        {
            const int i = CellCoord(m_px[3 * p], m_settings.origin[0], h, nx);
            const int k = CellCoord(m_px[3 * p + 2], m_settings.origin[2], h, nz);
            float& top = maxY[i + size_t(nx) * k];
            top = (std::max)(top, m_px[3 * p + 1]);
            const float vx = m_pv[3 * p];
            const float vy = m_pv[3 * p + 1];
            const float vz = m_pv[3 * p + 2];
            maxSpeed = (std::max)(maxSpeed, std::sqrt(vx * vx + vy * vy + vz * vz));
        }

        const float halfSpacing = 0.5f * h / static_cast<float>((std::max)(m_settings.particlesPerAxis, 1u));
        std::vector<float> raw(columns, 0.0f);
        m_columnWet.assign(columns, 0);
        for (size_t c = 0; c < columns; ++c)
        {
            if (maxY[c] > lowest)
            {
                m_columnWet[c] = 1;
                raw[c] = maxY[c] + halfSpacing;
            }
        }

        m_columnHeight.assign(columns, 0.0f);
        float surfaceMin = std::numeric_limits<float>::max();
        float surfaceMax = lowest;
        for (uint32_t k = 0; k < nz; ++k)
        {
            for (uint32_t i = 0; i < nx; ++i)
            {
                const size_t c = i + size_t(nx) * k;
                if (!m_columnWet[c])
                {
                    const float cx = m_settings.origin[0] + h * (i + 0.5f);
                    const float cz = m_settings.origin[2] + h * (k + 0.5f);
                    m_columnHeight[c] = (std::max)(SeaBed(cx, cz), m_settings.origin[1]);
                    continue;
                }
                float sum = 0.0f;
                int n = 0;
                for (int dk = -1; dk <= 1; ++dk)
                {
                    for (int di = -1; di <= 1; ++di)
                    {
                        const int ni = static_cast<int>(i) + di;
                        const int nk = static_cast<int>(k) + dk;
                        if (ni < 0 || nk < 0 || ni >= static_cast<int>(nx) || nk >= static_cast<int>(nz))
                        {
                            continue;
                        }
                        const size_t nc = ni + size_t(nx) * nk;
                        if (m_columnWet[nc])
                        {
                            sum += raw[nc];
                            ++n;
                        }
                    }
                }
                m_columnHeight[c] = sum / static_cast<float>(n);
                surfaceMin = (std::min)(surfaceMin, m_columnHeight[c]);
                surfaceMax = (std::max)(surfaceMax, m_columnHeight[c]);
            }
        }
        if (surfaceMax < surfaceMin)
        {
            surfaceMin = m_settings.origin[1];
            surfaceMax = m_settings.origin[1];
        }
        m_stats.particles = ParticleCount();
        m_stats.maxSpeed = maxSpeed;
        m_stats.surfaceMin = surfaceMin;
        m_stats.surfaceMax = surfaceMax;
    }

    float FlipWaterSolver::SampleSurfaceHeight(float x, float z) const
    {
        const uint32_t nx = m_settings.nx;
        const uint32_t nz = m_settings.nz;
        if (m_columnHeight.empty() || nx == 0 || nz == 0)
        {
            return m_settings.origin[1];
        }
        const float invH = 1.0f / m_settings.cellSize;
        const float fx = (std::max)(0.0f, (std::min)((x - m_settings.origin[0]) * invH - 0.5f, static_cast<float>(nx - 1)));
        const float fz = (std::max)(0.0f, (std::min)((z - m_settings.origin[2]) * invH - 0.5f, static_cast<float>(nz - 1)));
        const uint32_t i0 = (std::min)(static_cast<uint32_t>(fx), nx - 1);
        const uint32_t k0 = (std::min)(static_cast<uint32_t>(fz), nz - 1);
        const uint32_t i1 = (std::min)(i0 + 1, nx - 1);
        const uint32_t k1 = (std::min)(k0 + 1, nz - 1);
        const float tx = fx - static_cast<float>(i0);
        const float tz = fz - static_cast<float>(k0);
        const float h00 = m_columnHeight[i0 + size_t(nx) * k0];
        const float h10 = m_columnHeight[i1 + size_t(nx) * k0];
        const float h01 = m_columnHeight[i0 + size_t(nx) * k1];
        const float h11 = m_columnHeight[i1 + size_t(nx) * k1];
        return (h00 * (1.0f - tx) + h10 * tx) * (1.0f - tz) + (h01 * (1.0f - tx) + h11 * tx) * tz;
    }
}
