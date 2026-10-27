#include "Renderer/Voxel/PressureProjector.h"

#include "Foundation/Jobs/ParallelFor.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/StaggeredVelocityGrid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        enum CellType : uint8_t
        {
            CELL_SOLID = 0,
            CELL_FLUID = 1,
            CELL_ENCLOSED = 2
        };

        // Cell classification plus the neighbour queries every stage shares.
        // Cell index = x + dimX * (y + dimY * z).
        struct CellGrid
        {
            int dimX = 0;
            int dimY = 0;
            int dimZ = 0;
            bool solidFloor = true;
            std::vector<uint8_t> type;

            size_t Index(int x, int y, int z) const
            {
                return static_cast<size_t>(x) + static_cast<size_t>(dimX) * (static_cast<size_t>(y) + static_cast<size_t>(dimY) * static_cast<size_t>(z));
            }

            bool IsFluid(int x, int y, int z) const
            {
                if (x < 0 || y < 0 || z < 0 || x >= dimX || y >= dimY || z >= dimZ)
                {
                    return false;
                }
                return type[Index(x, y, z)] == CELL_FLUID;
            }

            // Off-diagonal entries of A for fluid cell (x,y,z): -1 when the + neighbour is fluid, else 0.
            // Returns 0 for a cell that is not fluid itself.
            float PlusI(int x, int y, int z) const { return IsFluid(x, y, z) && IsFluid(x + 1, y, z) ? -1.0f : 0.0f; }
            float PlusJ(int x, int y, int z) const { return IsFluid(x, y, z) && IsFluid(x, y + 1, z) ? -1.0f : 0.0f; }
            float PlusK(int x, int y, int z) const { return IsFluid(x, y, z) && IsFluid(x, y, z + 1) ? -1.0f : 0.0f; }

            // Number of non-solid neighbours: fluid cells and open ghost cells.
            int DiagonalCount(int x, int y, int z) const
            {
                int count = 0;
                const int offsets[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
                for (const auto& o : offsets)
                {
                    const int nx = x + o[0];
                    const int ny = y + o[1];
                    const int nz = z + o[2];
                    const bool inside = nx >= 0 && ny >= 0 && nz >= 0 && nx < dimX && ny < dimY && nz < dimZ;
                    if (inside)
                    {
                        if (type[Index(nx, ny, nz)] == CELL_FLUID)
                        {
                            ++count;
                        }
                    }
                    else if (!(ny < 0 && solidFloor))
                    {
                        ++count;
                    }
                }
                return count;
            }
        };

        bool IsOpenBoundaryCell(const CellGrid& g, int x, int y, int z)
        {
            return x == 0 || x == g.dimX - 1 || z == 0 || z == g.dimZ - 1 || y == g.dimY - 1 || (y == 0 && !g.solidFloor);
        }

        void ClassifyCells(const OccupancyBitGrid& solid, CellGrid& g, PressureProjectionStats& stats)
        {
            const size_t count = static_cast<size_t>(g.dimX) * g.dimY * g.dimZ;
            g.type.assign(count, CELL_ENCLOSED);
            std::vector<size_t> stack;
            for (int z = 0; z < g.dimZ; ++z)
            {
                for (int y = 0; y < g.dimY; ++y)
                {
                    for (int x = 0; x < g.dimX; ++x)
                    {
                        const size_t i = g.Index(x, y, z);
                        if (solid.Test(x, y, z))
                        {
                            g.type[i] = CELL_SOLID;
                        }
                        else if (IsOpenBoundaryCell(g, x, y, z))
                        {
                            g.type[i] = CELL_FLUID;
                            stack.push_back(i);
                        }
                    }
                }
            }

            const size_t strideY = static_cast<size_t>(g.dimX);
            const size_t strideZ = strideY * g.dimY;
            while (!stack.empty())
            {
                const size_t i = stack.back();
                stack.pop_back();
                const int x = static_cast<int>(i % strideY);
                const int y = static_cast<int>((i / strideY) % g.dimY);
                const int z = static_cast<int>(i / strideZ);
                const auto visit = [&](size_t n)
                {
                    if (g.type[n] == CELL_ENCLOSED)
                    {
                        g.type[n] = CELL_FLUID;
                        stack.push_back(n);
                    }
                };
                if (x > 0) visit(i - 1);
                if (x + 1 < g.dimX) visit(i + 1);
                if (y > 0) visit(i - strideY);
                if (y + 1 < g.dimY) visit(i + strideY);
                if (z > 0) visit(i - strideZ);
                if (z + 1 < g.dimZ) visit(i + strideZ);
            }

            for (const uint8_t t : g.type)
            {
                if (t == CELL_FLUID) ++stats.fluidCells;
                else if (t == CELL_SOLID) ++stats.solidCells;
                else ++stats.enclosedCells;
            }
        }

        // Calls fn(velocityRef, minusCell, plusCell, isWall) for every face of every axis. A cell index
        // of -1 is outside the domain (open ghost). isWall marks the y = 0 faces of a solid floor.
        template <typename Fn>
        void ForEachFace(const CellGrid& g, StaggeredVelocityGrid& vel, Fn&& fn)
        {
            const std::ptrdiff_t none = -1;
            for (int axis = 0; axis < 3; ++axis)
            {
                const int fx = g.dimX + (axis == 0 ? 1 : 0);
                const int fy = g.dimY + (axis == 1 ? 1 : 0);
                const int fz = g.dimZ + (axis == 2 ? 1 : 0);
                const int axisDim = axis == 0 ? g.dimX : (axis == 1 ? g.dimY : g.dimZ);
                for (int k = 0; k < fz; ++k)
                {
                    for (int j = 0; j < fy; ++j)
                    {
                        for (int i = 0; i < fx; ++i)
                        {
                            const int c = axis == 0 ? i : (axis == 1 ? j : k);
                            const std::ptrdiff_t plus = c < axisDim ? static_cast<std::ptrdiff_t>(g.Index(i, j, k)) : none;
                            const std::ptrdiff_t minus = c > 0
                                ? static_cast<std::ptrdiff_t>(g.Index(i - (axis == 0 ? 1 : 0), j - (axis == 1 ? 1 : 0), k - (axis == 2 ? 1 : 0)))
                                : none;
                            const bool wall = axis == 1 && c == 0 && g.solidFloor;
                            float& v = axis == 0 ? vel.U(i, j, k) : (axis == 1 ? vel.V(i, j, k) : vel.W(i, j, k));
                            fn(v, minus, plus, wall);
                        }
                    }
                }
            }
        }

        bool FaceBlocked(const CellGrid& g, std::ptrdiff_t minus, std::ptrdiff_t plus, bool wall)
        {
            return wall
                || (minus >= 0 && g.type[minus] != CELL_FLUID)
                || (plus >= 0 && g.type[plus] != CELL_FLUID);
        }

        // The 7-point matrix A, stored once instead of re-derived from cell types on every PCG iteration.
        // diag = A_ii (non-solid neighbour count); plus bit 1/2/4 set when A_(i,i+x/y/z) = -1 (Bridson's Aplusi/j/k).
        struct PoissonMatrix
        {
            std::vector<uint8_t> diag;
            std::vector<uint8_t> plus;
        };

        constexpr uint8_t PLUS_X = 1;
        constexpr uint8_t PLUS_Y = 2;
        constexpr uint8_t PLUS_Z = 4;

        void BuildPoissonMatrix(const CellGrid& g, PoissonMatrix& a)
        {
            a.diag.assign(g.type.size(), 0);
            a.plus.assign(g.type.size(), 0);
            Foundation::ParallelFor(static_cast<uint32_t>(g.dimZ), [&](uint32_t zIndex)
            {
                const int z = static_cast<int>(zIndex);
                for (int y = 0; y < g.dimY; ++y)
                {
                    for (int x = 0; x < g.dimX; ++x)
                    {
                        const size_t i = g.Index(x, y, z);
                        if (g.type[i] != CELL_FLUID)
                        {
                            continue;
                        }
                        a.diag[i] = static_cast<uint8_t>(g.DiagonalCount(x, y, z));
                        uint8_t bits = 0;
                        if (g.IsFluid(x + 1, y, z)) bits |= PLUS_X;
                        if (g.IsFluid(x, y + 1, z)) bits |= PLUS_Y;
                        if (g.IsFluid(x, y, z + 1)) bits |= PLUS_Z;
                        a.plus[i] = bits;
                    }
                }
            });
        }

        // Number of z slabs actually used: settings.micSlabs clamped to [1, dimZ].
        uint32_t SlabCount(const CellGrid& g, const PressureProjectionSettings& settings)
        {
            return std::clamp<uint32_t>(settings.micSlabs, 1u, static_cast<uint32_t>((std::max)(g.dimZ, 1)));
        }

        // First z slice of `slab` (slab == slabs gives dimZ).
        int SlabBeginZ(const CellGrid& g, uint32_t slab, uint32_t slabs)
        {
            return static_cast<int>(static_cast<uint64_t>(slab) * static_cast<uint64_t>(g.dimZ) / slabs);
        }

        // Applies z = M^-1 r with the MIC(0) factor (Bridson & Mueller-Fischer 2007, Fig. 4.3).
        // precon[i] == 0 marks every cell outside the solve. With slabs > 1 the factor is block-diagonal
        // (one block per z slab, the z couplings across slab borders dropped; block Jacobi with an
        // incomplete factor per block, Saad 2003 ch. 12), so the slabs solve in parallel. slabs = 1 is
        // the original sequential solve, bit for bit.
        void ApplyPreconditioner(const CellGrid& g, const PoissonMatrix& a, const std::vector<float>& precon,
                                 uint32_t slabs, const std::vector<float>& r, std::vector<float>& q, std::vector<float>& z)
        {
            const size_t strideY = static_cast<size_t>(g.dimX);
            const size_t strideZ = strideY * g.dimY;
            Foundation::ParallelFor(slabs, [&](uint32_t slab)
            {
                const size_t begin = static_cast<size_t>(SlabBeginZ(g, slab, slabs)) * strideZ;
                const size_t end = static_cast<size_t>(SlabBeginZ(g, slab + 1, slabs)) * strideZ;
                for (size_t i = begin; i < end; ++i)
                {
                    if (precon[i] == 0.0f)
                    {
                        q[i] = 0.0f;
                        continue;
                    }
                    // A_(i-1,i) is nonzero only when i-1 is in the same row, so the plus bits stand in for the bounds checks.
                    float t = r[i];
                    if (i >= 1 && (a.plus[i - 1] & PLUS_X)) t += precon[i - 1] * q[i - 1];
                    if (i >= strideY && (a.plus[i - strideY] & PLUS_Y)) t += precon[i - strideY] * q[i - strideY];
                    if (i >= begin + strideZ && (a.plus[i - strideZ] & PLUS_Z)) t += precon[i - strideZ] * q[i - strideZ];
                    q[i] = t * precon[i];
                }
                for (size_t n = end; n-- > begin;)
                {
                    if (precon[n] == 0.0f)
                    {
                        z[n] = 0.0f;
                        continue;
                    }
                    const uint8_t bits = a.plus[n];
                    float t = q[n];
                    if (bits & PLUS_X) t += precon[n] * z[n + 1];
                    if (bits & PLUS_Y) t += precon[n] * z[n + strideY];
                    if ((bits & PLUS_Z) && n + strideZ < end) t += precon[n] * z[n + strideZ];
                    z[n] = t * precon[n];
                }
            });
        }

        // Runs fn(begin, end, slice) over one z slice of cells per job.
        template <typename Func>
        void ForEachSlice(const CellGrid& g, Func&& fn)
        {
            const size_t sliceSize = static_cast<size_t>(g.dimX) * g.dimY;
            Foundation::ParallelFor(static_cast<uint32_t>(g.dimZ), [&](uint32_t slice)
            {
                fn(slice * sliceSize, (slice + 1) * sliceSize, slice);
            });
        }

        // out = A s over the fluid cells: A_ii s_i - sum of fluid neighbours.
        void MultiplyA(const CellGrid& g, const PoissonMatrix& a, const std::vector<float>& s, std::vector<float>& out)
        {
            const size_t strideY = static_cast<size_t>(g.dimX);
            const size_t strideZ = strideY * g.dimY;
            ForEachSlice(g, [&](size_t begin, size_t end, uint32_t)
            {
                for (size_t i = begin; i < end; ++i)
                {
                    if (g.type[i] != CELL_FLUID)
                    {
                        out[i] = 0.0f;
                        continue;
                    }
                    const uint8_t bits = a.plus[i];
                    float v = static_cast<float>(a.diag[i]) * s[i];
                    if (i >= 1 && (a.plus[i - 1] & PLUS_X)) v -= s[i - 1];
                    if (bits & PLUS_X) v -= s[i + 1];
                    if (i >= strideY && (a.plus[i - strideY] & PLUS_Y)) v -= s[i - strideY];
                    if (bits & PLUS_Y) v -= s[i + strideY];
                    if (i >= strideZ && (a.plus[i - strideZ] & PLUS_Z)) v -= s[i - strideZ];
                    if (bits & PLUS_Z) v -= s[i + strideZ];
                    out[i] = v;
                }
            });
        }

        // Per-slice partial sums added in slice order, so the result does not depend on job scheduling.
        double Dot(const CellGrid& g, const std::vector<float>& a, const std::vector<float>& b)
        {
            std::vector<double> partial(static_cast<size_t>(g.dimZ), 0.0);
            ForEachSlice(g, [&](size_t begin, size_t end, uint32_t slice)
            {
                double sum = 0.0;
                for (size_t i = begin; i < end; ++i)
                {
                    sum += static_cast<double>(a[i]) * static_cast<double>(b[i]);
                }
                partial[slice] = sum;
            });
            double sum = 0.0;
            for (const double value : partial)
            {
                sum += value;
            }
            return sum;
        }

        float MaxAbs(const std::vector<float>& a)
        {
            float m = 0.0f;
            for (const float v : a)
            {
                m = std::max(m, std::fabs(v));
            }
            return m;
        }

        float MaxFluidDivergence(const CellGrid& g, const StaggeredVelocityGrid& vel)
        {
            float m = 0.0f;
            for (int z = 0; z < g.dimZ; ++z)
            {
                for (int y = 0; y < g.dimY; ++y)
                {
                    for (int x = 0; x < g.dimX; ++x)
                    {
                        if (g.type[g.Index(x, y, z)] == CELL_FLUID)
                        {
                            m = std::max(m, std::fabs(vel.Divergence(x, y, z)));
                        }
                    }
                }
            }
            return m;
        }

        // MIC(0) diagonal factors (Fig. 4.2; tau as printed there). Cells with no non-solid neighbour get 0 and drop out of the solve.
        // Each z slab is factored on its own (the z term is skipped on a slab's first slice), in parallel.
        void BuildPreconditioner(const CellGrid& g, const PoissonMatrix& a, const PressureProjectionSettings& settings,
                                 uint32_t slabs, std::vector<float>& precon)
        {
            const size_t strideY = static_cast<size_t>(g.dimX);
            const size_t strideZ = strideY * g.dimY;
            const double tau = settings.micTau;
            const double sigma = settings.micSigma;
            const auto coeff = [&](size_t i, uint8_t bit) { return (a.plus[i] & bit) ? -1.0 : 0.0; };
            precon.assign(g.type.size(), 0.0f);
            Foundation::ParallelFor(slabs, [&](uint32_t slab)
            {
            const int zBegin = SlabBeginZ(g, slab, slabs);
            const int zEnd = SlabBeginZ(g, slab + 1, slabs);
            for (int z = zBegin; z < zEnd; ++z)
            {
                // The block drops A_(i,i+z) on the slab's last slice, so its MIC fill-in term goes too.
                const double zKeep = z + 1 < zEnd ? 1.0 : 0.0;
                for (int y = 0; y < g.dimY; ++y)
                {
                    for (int x = 0; x < g.dimX; ++x)
                    {
                        const size_t i = g.Index(x, y, z);
                        if (g.type[i] != CELL_FLUID)
                        {
                            continue;
                        }
                        const double diag = a.diag[i];
                        if (diag == 0.0)
                        {
                            continue;
                        }
                        double e = diag;
                        if (x > 0)
                        {
                            const size_t n = i - 1;
                            const double p = precon[n];
                            const double ai = coeff(n, PLUS_X);
                            const double aj = coeff(n, PLUS_Y);
                            const double ak = zKeep * coeff(n, PLUS_Z);
                            e -= (ai * p) * (ai * p) + tau * (ai * (aj + ak) * p * p);
                        }
                        if (y > 0)
                        {
                            const size_t n = i - strideY;
                            const double p = precon[n];
                            const double ai = coeff(n, PLUS_X);
                            const double aj = coeff(n, PLUS_Y);
                            const double ak = zKeep * coeff(n, PLUS_Z);
                            e -= (aj * p) * (aj * p) + tau * (aj * (ai + ak) * p * p);
                        }
                        if (z > zBegin)
                        {
                            const size_t n = i - strideZ;
                            const double p = precon[n];
                            const double ai = coeff(n, PLUS_X);
                            const double aj = coeff(n, PLUS_Y);
                            const double ak = coeff(n, PLUS_Z);
                            e -= (ak * p) * (ak * p) + tau * (ak * (ai + aj) * p * p);
                        }
                        if (e < sigma * diag)
                        {
                            e = diag;
                        }
                        precon[i] = static_cast<float>(1.0 / std::sqrt(e));
                    }
                }
            }
            });
        }
    }

    PressureProjectionStats ProjectDivergenceFree(const OccupancyBitGrid& solid, const PressureProjectionSettings& settings,
                                                  StaggeredVelocityGrid& inOutVelocity, std::vector<float>* inOutPressure)
    {
        PressureProjectionStats stats;
        const auto solveStart = std::chrono::steady_clock::now();
        const auto elapsedMs = [](std::chrono::steady_clock::time_point since)
        {
            return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - since).count();
        };
        if (solid.VoxelCount() == 0 || solid.DimX() != inOutVelocity.DimX() || solid.DimY() != inOutVelocity.DimY()
            || solid.DimZ() != inOutVelocity.DimZ())
        {
            return stats;
        }
        stats.valid = true;

        CellGrid g;
        g.dimX = static_cast<int>(solid.DimX());
        g.dimY = static_cast<int>(solid.DimY());
        g.dimZ = static_cast<int>(solid.DimZ());
        g.solidFloor = settings.solidFloor;
        ClassifyCells(solid, g, stats);

        ForEachFace(g, inOutVelocity, [&](float& v, std::ptrdiff_t minus, std::ptrdiff_t plus, bool wall)
        {
            if (FaceBlocked(g, minus, plus, wall))
            {
                v = 0.0f;
            }
        });

        const size_t cellCount = g.type.size();
        std::vector<float> r(cellCount, 0.0f);
        for (int z = 0; z < g.dimZ; ++z)
        {
            for (int y = 0; y < g.dimY; ++y)
            {
                for (int x = 0; x < g.dimX; ++x)
                {
                    const size_t i = g.Index(x, y, z);
                    if (g.type[i] == CELL_FLUID && g.DiagonalCount(x, y, z) > 0)
                    {
                        const float div = inOutVelocity.Divergence(x, y, z);
                        r[i] = -div;
                        stats.initialMaxDivergence = std::max(stats.initialMaxDivergence, std::fabs(div));
                    }
                }
            }
        }

        if (stats.initialMaxDivergence == 0.0f)
        {
            stats.converged = true;
            stats.finalMaxDivergence = MaxFluidDivergence(g, inOutVelocity);
            if (inOutPressure)
            {
                inOutPressure->assign(cellCount, 0.0f);
            }
            stats.solveMs = elapsedMs(solveStart);
            return stats;
        }

        PoissonMatrix a;
        BuildPoissonMatrix(g, a);
        const uint32_t slabs = SlabCount(g, settings);
        stats.micSlabs = slabs;
        std::vector<float> precon;
        BuildPreconditioner(g, a, settings, slabs, precon);

        std::vector<float> p(cellCount, 0.0f);
        std::vector<float> q(cellCount, 0.0f);
        std::vector<float> z(cellCount, 0.0f);
        std::vector<float> s(cellCount, 0.0f);
        std::vector<float> as(cellCount, 0.0f);

        // Warm start: r = b - A p0. Any p0 is valid, only the iteration count depends on it.
        if (inOutPressure && inOutPressure->size() == cellCount)
        {
            for (size_t i = 0; i < cellCount; ++i)
            {
                p[i] = precon[i] != 0.0f ? (*inOutPressure)[i] : 0.0f;
            }
            MultiplyA(g, a, p, as);
            ForEachSlice(g, [&](size_t begin, size_t end, uint32_t)
            {
                for (size_t i = begin; i < end; ++i)
                {
                    r[i] -= as[i];
                }
            });
            stats.warmStarted = true;
        }
        const float stopResidual = settings.tolerance * stats.initialMaxDivergence;
        stats.converged = MaxAbs(r) <= stopResidual;

        auto preconStart = std::chrono::steady_clock::now();
        ApplyPreconditioner(g, a, precon, slabs, r, q, z);
        stats.preconditionerMs += elapsedMs(preconStart);
        s = z;
        double sigma = Dot(g, z, r);

        for (uint32_t iter = 0; iter < settings.maxIterations && !stats.converged; ++iter)
        {
            if (sigma == 0.0)
            {
                break;
            }
            MultiplyA(g, a, s, as);
            const double denom = Dot(g, s, as);
            if (!(denom > 0.0))
            {
                break;
            }
            const float alpha = static_cast<float>(sigma / denom);
            ForEachSlice(g, [&](size_t begin, size_t end, uint32_t)
            {
                for (size_t i = begin; i < end; ++i)
                {
                    p[i] += alpha * s[i];
                    r[i] -= alpha * as[i];
                }
            });
            stats.iterations = iter + 1;
            if (MaxAbs(r) <= stopResidual)
            {
                stats.converged = true;
                break;
            }
            preconStart = std::chrono::steady_clock::now();
            ApplyPreconditioner(g, a, precon, slabs, r, q, z);
            stats.preconditionerMs += elapsedMs(preconStart);
            const double sigmaNew = Dot(g, z, r);
            const float beta = static_cast<float>(sigmaNew / sigma);
            ForEachSlice(g, [&](size_t begin, size_t end, uint32_t)
            {
                for (size_t i = begin; i < end; ++i)
                {
                    s[i] = z[i] + beta * s[i];
                }
            });
            sigma = sigmaNew;
        }

        ForEachFace(g, inOutVelocity, [&](float& v, std::ptrdiff_t minus, std::ptrdiff_t plus, bool wall)
        {
            if (FaceBlocked(g, minus, plus, wall))
            {
                return;
            }
            const float pPlus = plus >= 0 ? p[plus] : 0.0f;
            const float pMinus = minus >= 0 ? p[minus] : 0.0f;
            v -= pPlus - pMinus;
        });

        stats.finalMaxDivergence = MaxFluidDivergence(g, inOutVelocity);
        if (inOutPressure)
        {
            *inOutPressure = std::move(p);
        }
        stats.solveMs = elapsedMs(solveStart);
        return stats;
    }
}
