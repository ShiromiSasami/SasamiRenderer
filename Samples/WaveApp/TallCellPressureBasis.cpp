#include "TallCellPressureBasis.h"

#include "QuadtreeColumnLayout.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        struct BasisEntry
        {
            int32_t row = -1;
            double weight = 0.0;
        };

        struct CellBasis
        {
            BasisEntry e[2];
            int count = 0;
        };

        struct Coef
        {
            uint32_t col;
            double val;
        };

        struct Csr
        {
            std::vector<uint32_t> rowStart;
            std::vector<uint32_t> col;
            std::vector<double> val;
        };

        FlipCell NeighbourType(const FlipCellGrid& cells, int x, int y, int z)
        {
            if (y >= int(cells.ny))
                return FlipCell::Air;
            if (x < 0 || x >= int(cells.nx) || y < 0 || z < 0 || z >= int(cells.nz))
                return FlipCell::Solid;
            return cells.At(uint32_t(x), uint32_t(y), uint32_t(z));
        }

        float MsSince(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        double MaxAbs(const std::vector<double>& v)
        {
            double m = 0.0;
            for (double d : v)
                m = (std::max)(m, std::abs(d));
            return m;
        }

        void Multiply(const Csr& a, const std::vector<double>& x, std::vector<double>& y)
        {
            const size_t n = a.rowStart.size() - 1;
            for (size_t i = 0; i < n; ++i)
            {
                double s = 0.0;
                for (uint32_t k = a.rowStart[i]; k < a.rowStart[i + 1]; ++k)
                    s += a.val[k] * x[a.col[k]];
                y[i] = s;
            }
        }

        // Zero fill-in incomplete Cholesky. Rows hold the lower triangle, diagonal last.
        Csr FactorIc0(const Csr& a)
        {
            const size_t n = a.rowStart.size() - 1;
            Csr l;
            l.rowStart.assign(n + 1, 0);
            for (size_t i = 0; i < n; ++i)
            {
                uint32_t count = 0;
                for (uint32_t k = a.rowStart[i]; k < a.rowStart[i + 1]; ++k)
                    if (a.col[k] <= i)
                        ++count;
                l.rowStart[i + 1] = l.rowStart[i] + count;
            }
            l.col.resize(l.rowStart[n]);
            l.val.resize(l.rowStart[n]);

            for (size_t i = 0; i < n; ++i)
            {
                uint32_t out = l.rowStart[i];
                double diagA = 0.0;
                for (uint32_t k = a.rowStart[i]; k < a.rowStart[i + 1]; ++k)
                {
                    if (a.col[k] > i)
                        continue;
                    l.col[out] = a.col[k];
                    l.val[out] = a.val[k];
                    if (a.col[k] == i)
                        diagA = a.val[k];
                    ++out;
                }
                const uint32_t rowBegin = l.rowStart[i];
                const uint32_t rowEnd = l.rowStart[i + 1];
                double sumSq = 0.0;
                for (uint32_t p = rowBegin; p + 1 < rowEnd; ++p)
                {
                    const uint32_t k = l.col[p];
                    double s = l.val[p];
                    uint32_t pi = rowBegin;
                    uint32_t pk = l.rowStart[k];
                    const uint32_t kEnd = l.rowStart[k + 1] - 1;   // exclude L_kk
                    while (pi < p && pk < kEnd)
                    {
                        if (l.col[pi] == l.col[pk])
                        {
                            s -= l.val[pi] * l.val[pk];
                            ++pi;
                            ++pk;
                        }
                        else if (l.col[pi] < l.col[pk])
                            ++pi;
                        else
                            ++pk;
                    }
                    const double lkk = l.val[kEnd];
                    const double lik = lkk != 0.0 ? s / lkk : 0.0;
                    l.val[p] = lik;
                    sumSq += lik * lik;
                }
                const double under = diagA - sumSq;
                l.val[rowEnd - 1] = (under >= 0.25 * diagA && std::isfinite(under)) ? std::sqrt(under) : std::sqrt(diagA);
            }
            return l;
        }

        // z = (L L^T)^-1 r; `z` is used in place.
        void ApplyIc0(const Csr& l, const std::vector<double>& r, std::vector<double>& z)
        {
            const size_t n = l.rowStart.size() - 1;
            for (size_t i = 0; i < n; ++i)
            {
                double s = r[i];
                const uint32_t last = l.rowStart[i + 1] - 1;
                for (uint32_t k = l.rowStart[i]; k < last; ++k)
                    s -= l.val[k] * z[l.col[k]];
                z[i] = s / l.val[last];
            }
            for (size_t ii = n; ii-- > 0;)
            {
                const uint32_t last = l.rowStart[ii + 1] - 1;
                const double xi = z[ii] / l.val[last];
                z[ii] = xi;
                for (uint32_t k = l.rowStart[ii]; k < last; ++k)
                    z[l.col[k]] -= l.val[k] * xi;
            }
        }
    }

    TallCellLayout BuildTallCellLayout(const FlipCellGrid& cells, uint32_t bandCells, uint32_t minTallCells,
                                       const std::vector<uint16_t>* columnBands)
    {
        const bool perColumn = columnBands && columnBands->size() == size_t(cells.nx) * cells.nz;
        TallCellLayout layout;
        layout.nx = cells.nx;
        layout.nz = cells.nz;
        layout.columns.assign(size_t(cells.nx) * cells.nz, TallCellColumn{});
        const int minTall = int((std::max)(minTallCells, 2u));

        for (uint32_t z = 0; z < cells.nz; ++z)
        {
            for (uint32_t x = 0; x < cells.nx; ++x)
            {
                uint32_t bottom = 0;
                while (bottom < cells.ny && cells.At(x, bottom, z) == FlipCell::Solid)
                    ++bottom;
                if (bottom >= cells.ny || cells.At(x, bottom, z) != FlipCell::Fluid)
                    continue;
                uint32_t topFluid = bottom;
                while (topFluid + 1 < cells.ny && cells.At(x, topFluid + 1, z) == FlipCell::Fluid)
                    ++topFluid;
                const uint32_t band = perColumn ? (*columnBands)[x + size_t(cells.nx) * z] : bandCells;
                const int top = int(topFluid) + 1 - int(band);
                if (top <= int(bottom))
                    continue;
                if (top - int(bottom) >= minTall)
                {
                    layout.columns[x + size_t(cells.nx) * z] = TallCellColumn{uint16_t(bottom), uint16_t(top)};
                    ++layout.tallColumns;
                    layout.mergedCells += uint32_t(top) - bottom;
                }
            }
        }
        return layout;
    }

    void RestrictTallCellsToLeaves(TallCellLayout& layout, const QuadtreeColumnLayout& quadtree, uint32_t minTallCells)
    {
        if (layout.nx != quadtree.nx || layout.nz != quadtree.nz ||
            layout.columns.size() != size_t(layout.nx) * layout.nz || quadtree.leafOf.size() != layout.columns.size())
        {
            return;
        }
        const uint32_t minTall = (std::max)(minTallCells, 2u);
        layout.tallColumns = 0;
        layout.mergedCells = 0;
        for (const QuadtreeLeaf& leaf : quadtree.leaves)
        {
            const uint32_t x1 = (std::min)(leaf.x + leaf.size, layout.nx);
            const uint32_t z1 = (std::min)(leaf.z + leaf.size, layout.nz);
            uint32_t b = 0;
            uint32_t t = 0xFFFFu;
            for (uint32_t z = leaf.z; z < z1; ++z)
            {
                for (uint32_t x = leaf.x; x < x1; ++x)
                {
                    const TallCellColumn& c = layout.columns[x + size_t(layout.nx) * z];
                    if (c.top < c.bottom + 2)
                    {
                        t = 0;
                        continue;
                    }
                    b = (std::max)(b, uint32_t(c.bottom));
                    t = (std::min)(t, uint32_t(c.top));
                }
            }
            const bool keep = t >= b + minTall;
            for (uint32_t z = leaf.z; z < z1; ++z)
            {
                for (uint32_t x = leaf.x; x < x1; ++x)
                {
                    TallCellColumn& c = layout.columns[x + size_t(layout.nx) * z];
                    c = keep ? TallCellColumn{uint16_t(b), uint16_t(t)} : TallCellColumn{};
                    if (keep)
                    {
                        ++layout.tallColumns;
                        layout.mergedCells += t - b;
                    }
                }
            }
        }
    }

    PressureSolveStats SolveTallCellPressure(const FlipCellGrid& cells, const TallCellLayout& layout,
                                             const std::vector<float>& divergence, std::vector<float>& pressure,
                                             const PressureSolveSettings& settings, const QuadtreeColumnLayout* quadtree)
    {
        PressureSolveStats stats;
        const auto buildStart = std::chrono::steady_clock::now();
        const size_t cellCount = cells.CellCount();
        pressure.assign(cellCount, 0.0f);

        // Effective tall range per column (validated against the grid).
        const size_t columnCount = size_t(cells.nx) * cells.nz;
        std::vector<uint32_t> tallB(columnCount, 0), tallT(columnCount, 0);
        std::vector<int32_t> rowB(columnCount, -1), rowT(columnCount, -1);
        if (layout.nx == cells.nx && layout.nz == cells.nz && layout.columns.size() == columnCount)
        {
            for (size_t col = 0; col < columnCount; ++col)
            {
                const uint32_t b = layout.columns[col].bottom;
                const uint32_t t = layout.columns[col].top;
                if (t < b + 2 || t > cells.ny)
                    continue;
                const uint32_t x = uint32_t(col % cells.nx);
                const uint32_t z = uint32_t(col / cells.nx);
                bool ok = true;
                for (uint32_t y = b; y < t && ok; ++y)
                    ok = cells.At(x, y, z) == FlipCell::Fluid;
                if (ok)
                {
                    tallB[col] = b;
                    tallT[col] = t;
                }
            }
        }

        // Column groups: one per quadtree leaf, else one per column. A group's columns must share one tall
        // range; otherwise the group gets none.
        const bool useQuadtree = quadtree && quadtree->nx == cells.nx && quadtree->nz == cells.nz &&
                                 quadtree->leafOf.size() == columnCount;
        std::vector<uint32_t> groupOf(columnCount);
        for (size_t col = 0; col < columnCount; ++col)
            groupOf[col] = useQuadtree ? quadtree->leafOf[col] : uint32_t(col);
        if (useQuadtree)
        {
            const size_t groupCount = quadtree->leaves.size();
            std::vector<uint32_t> groupB(groupCount, 0), groupT(groupCount, 0);
            std::vector<uint8_t> groupSeen(groupCount, 0), groupBad(groupCount, 0);
            for (size_t col = 0; col < columnCount; ++col)
            {
                const uint32_t g = groupOf[col];
                if (!groupSeen[g])
                {
                    groupSeen[g] = 1;
                    groupB[g] = tallB[col];
                    groupT[g] = tallT[col];
                }
                else if (groupB[g] != tallB[col] || groupT[g] != tallT[col])
                {
                    groupBad[g] = 1;
                }
            }
            for (size_t col = 0; col < columnCount; ++col)
            {
                if (groupBad[groupOf[col]])
                {
                    tallB[col] = 0;
                    tallT[col] = 0;
                }
            }
        }

        // Basis: pass 1 assigns rows (first appearance in z, y, x order; uniform cells of one group and
        // layer share a row), pass 2 fills the interior tall-cell entries.
        std::vector<CellBasis> basis(cellCount);
        std::vector<int32_t> rowOf(columnCount * cells.ny, -1);
        uint32_t rawRows = 0;
        for (uint32_t z = 0; z < cells.nz; ++z)
        {
            for (uint32_t y = 0; y < cells.ny; ++y)
            {
                for (uint32_t x = 0; x < cells.nx; ++x)
                {
                    const size_t idx = cells.Index(x, y, z);
                    if (cells.type[idx] != uint8_t(FlipCell::Fluid))
                        continue;
                    const size_t col = x + size_t(cells.nx) * z;
                    const uint32_t g = groupOf[col];
                    const uint32_t b = tallB[col];
                    const uint32_t t = tallT[col];
                    CellBasis& cb = basis[idx];
                    int32_t& row = (t == 0 || y < b || y >= t) ? rowOf[size_t(g) * cells.ny + y]
                                   : (y == b)                  ? rowB[g]
                                   : (y == t - 1)              ? rowT[g]
                                                               : rowOf[size_t(g) * cells.ny + y];
                    if (t != 0 && y > b && y + 1 < t)
                        continue;
                    if (row < 0)
                        row = int32_t(rawRows++);
                    cb.e[0] = BasisEntry{row, 1.0};
                    cb.count = 1;
                }
            }
        }
        for (uint32_t z = 0; z < cells.nz; ++z)
        {
            for (uint32_t x = 0; x < cells.nx; ++x)
            {
                const size_t col = x + size_t(cells.nx) * z;
                const uint32_t b = tallB[col];
                const uint32_t t = tallT[col];
                if (t == 0)
                    continue;
                const uint32_t g = groupOf[col];
                const double invSpan = 1.0 / double(t - b - 1);
                for (uint32_t y = b + 1; y + 1 < t; ++y)
                {
                    const double s = double(y - b) * invSpan;
                    CellBasis& cb = basis[cells.Index(x, y, z)];
                    cb.e[0] = BasisEntry{rowB[g], 1.0 - s};
                    cb.e[1] = BasisEntry{rowT[g], s};
                    cb.count = 2;
                }
            }
        }

        // Galerkin assembly: Ar = W^T A W, br = W^T b.
        std::vector<std::vector<Coef>> acc(rawRows);
        std::vector<double> br(rawRows, 0.0);
        const int nx = int(cells.nx), ny = int(cells.ny), nz = int(cells.nz);
        static const int OFFSETS[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
        for (int z = 0; z < nz; ++z)
        {
            for (int y = 0; y < ny; ++y)
            {
                for (int x = 0; x < nx; ++x)
                {
                    const size_t idx = cells.Index(uint32_t(x), uint32_t(y), uint32_t(z));
                    const CellBasis& cb = basis[idx];
                    if (cb.count == 0)
                        continue;
                    const double bc = -double(divergence[idx]);
                    int diag = 0;
                    for (int d = 0; d < 6; ++d)
                    {
                        const int ox = x + OFFSETS[d][0], oy = y + OFFSETS[d][1], oz = z + OFFSETS[d][2];
                        const FlipCell nt = NeighbourType(cells, ox, oy, oz);
                        if (nt == FlipCell::Solid)
                            continue;
                        ++diag;
                        if (nt != FlipCell::Fluid)
                            continue;
                        const CellBasis& nb = basis[cells.Index(uint32_t(ox), uint32_t(oy), uint32_t(oz))];
                        for (int i = 0; i < cb.count; ++i)
                            for (int j = 0; j < nb.count; ++j)
                                acc[cb.e[i].row].push_back(
                                    Coef{uint32_t(nb.e[j].row), -cb.e[i].weight * nb.e[j].weight});
                    }
                    for (int i = 0; i < cb.count; ++i)
                    {
                        br[cb.e[i].row] += cb.e[i].weight * bc;
                        for (int j = 0; j < cb.count; ++j)
                            acc[cb.e[i].row].push_back(
                                Coef{uint32_t(cb.e[j].row), cb.e[i].weight * double(diag) * cb.e[j].weight});
                    }
                }
            }
        }

        // Combine duplicates; rows with a non-positive diagonal are dropped.
        std::vector<int32_t> compact(rawRows, -1);
        uint32_t n = 0;
        for (uint32_t r = 0; r < rawRows; ++r)
        {
            std::vector<Coef>& row = acc[r];
            std::sort(row.begin(), row.end(), [](const Coef& a, const Coef& b) { return a.col < b.col; });
            size_t out = 0;
            for (size_t k = 0; k < row.size(); ++k)
            {
                if (out > 0 && row[out - 1].col == row[k].col)
                    row[out - 1].val += row[k].val;
                else
                    row[out++] = row[k];
            }
            row.resize(out);
            double diagVal = 0.0;
            for (const Coef& c : row)
                if (c.col == r)
                    diagVal = c.val;
            if (diagVal > 0.0)
                compact[r] = int32_t(n++);
        }

        Csr a;
        a.rowStart.assign(size_t(n) + 1, 0);
        std::vector<double> rhs(n, 0.0);
        for (uint32_t r = 0; r < rawRows; ++r)
        {
            if (compact[r] < 0)
                continue;
            uint32_t count = 0;
            for (const Coef& c : acc[r])
                if (compact[c.col] >= 0)
                    ++count;
            a.rowStart[size_t(compact[r]) + 1] = count;
            rhs[compact[r]] = br[r];
        }
        for (uint32_t i = 0; i < n; ++i)
            a.rowStart[i + 1] += a.rowStart[i];
        a.col.resize(a.rowStart[n]);
        a.val.resize(a.rowStart[n]);
        for (uint32_t r = 0; r < rawRows; ++r)
        {
            if (compact[r] < 0)
                continue;
            uint32_t out = a.rowStart[compact[r]];
            for (const Coef& c : acc[r])
            {
                if (compact[c.col] < 0)
                    continue;
                a.col[out] = uint32_t(compact[c.col]);
                a.val[out] = c.val;
                ++out;
            }
        }
        acc.clear();
        acc.shrink_to_fit();

        const Csr l = FactorIc0(a);
        stats.unknowns = n;
        stats.buildMs = MsSince(buildStart);

        const auto solveStart = std::chrono::steady_clock::now();
        std::vector<double> x(n, 0.0);
        const double bMax = MaxAbs(rhs);
        if (bMax == 0.0)
        {
            stats.converged = true;
        }
        else
        {
            std::vector<double> r = rhs, z(n, 0.0), p(n, 0.0), q(n, 0.0);
            ApplyIc0(l, r, z);
            p = z;
            double rz = 0.0;
            for (uint32_t i = 0; i < n; ++i)
                rz += r[i] * z[i];
            const double target = double(settings.tolerance) * bMax;
            for (uint32_t iter = 0; iter < settings.maxIterations; ++iter)
            {
                Multiply(a, p, q);
                double pq = 0.0;
                for (uint32_t i = 0; i < n; ++i)
                    pq += p[i] * q[i];
                if (!(pq > 0.0))
                    break;
                const double alpha = rz / pq;
                for (uint32_t i = 0; i < n; ++i)
                {
                    x[i] += alpha * p[i];
                    r[i] -= alpha * q[i];
                }
                stats.iterations = iter + 1;
                if (MaxAbs(r) <= target)
                {
                    stats.converged = true;
                    break;
                }
                ApplyIc0(l, r, z);
                double rzNew = 0.0;
                for (uint32_t i = 0; i < n; ++i)
                    rzNew += r[i] * z[i];
                const double beta = rz != 0.0 ? rzNew / rz : 0.0;
                rz = rzNew;
                for (uint32_t i = 0; i < n; ++i)
                    p[i] = z[i] + beta * p[i];
            }
            stats.residual = float(MaxAbs(r) / bMax);
        }
        stats.solveMs = MsSince(solveStart);

        for (size_t idx = 0; idx < cellCount; ++idx)
        {
            const CellBasis& cb = basis[idx];
            double v = 0.0;
            for (int i = 0; i < cb.count; ++i)
            {
                const int32_t c = compact[cb.e[i].row];
                if (c >= 0)
                    v += cb.e[i].weight * x[c];
            }
            pressure[idx] = float(v);
        }
        return stats;
    }
}
