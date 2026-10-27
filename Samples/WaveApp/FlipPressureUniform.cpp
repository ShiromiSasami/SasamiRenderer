#include "FlipPressureSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr double MIC_TAU = 0.97;
        constexpr double MIC_SIGMA = 0.25;

        // Neighbour slots: -x, +x, -y, +y, -z, +z.
        struct PressureRow
        {
            double diag = 0.0;
            int32_t nbr[6] = {-1, -1, -1, -1, -1, -1};
        };

        float ElapsedMs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
        {
            return std::chrono::duration<float, std::milli>(to - from).count();
        }

        FlipCell Classify(const FlipCellGrid& cells, int x, int y, int z)
        {
            if (y >= int(cells.ny))
                return FlipCell::Air;
            if (x < 0 || y < 0 || z < 0 || x >= int(cells.nx) || z >= int(cells.nz))
                return FlipCell::Solid;
            return cells.At(uint32_t(x), uint32_t(y), uint32_t(z));
        }

        void BuildRows(const FlipCellGrid& cells, std::vector<int32_t>& cellToRow,
                       std::vector<PressureRow>& rows, std::vector<size_t>& rowCell)
        {
            cellToRow.assign(cells.CellCount(), -1);

            // Pass 1: count non-Solid neighbours and keep only rows that have any.
            static const int OFFSETS[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
            for (uint32_t z = 0; z < cells.nz; ++z)
            {
                for (uint32_t y = 0; y < cells.ny; ++y)
                {
                    for (uint32_t x = 0; x < cells.nx; ++x)
                    {
                        if (cells.At(x, y, z) != FlipCell::Fluid)
                            continue;
                        int open = 0;
                        for (const auto& o : OFFSETS)
                        {
                            if (Classify(cells, int(x) + o[0], int(y) + o[1], int(z) + o[2]) != FlipCell::Solid)
                                ++open;
                        }
                        if (open == 0)
                            continue;
                        const size_t cell = cells.Index(x, y, z);
                        cellToRow[cell] = int32_t(rows.size());
                        rowCell.push_back(cell);
                        PressureRow row;
                        row.diag = double(open);
                        rows.push_back(row);
                    }
                }
            }

            // Pass 2: link Fluid neighbours that became rows.
            for (size_t r = 0; r < rows.size(); ++r)
            {
                const size_t cell = rowCell[r];
                const int x = int(cell % cells.nx);
                const int y = int((cell / cells.nx) % cells.ny);
                const int z = int(cell / (size_t(cells.nx) * cells.ny));
                for (int s = 0; s < 6; ++s)
                {
                    const int nx = x + OFFSETS[s][0];
                    const int ny = y + OFFSETS[s][1];
                    const int nz = z + OFFSETS[s][2];
                    if (nx < 0 || ny < 0 || nz < 0 || nx >= int(cells.nx) || ny >= int(cells.ny) || nz >= int(cells.nz))
                        continue;
                    rows[r].nbr[s] = cellToRow[cells.Index(uint32_t(nx), uint32_t(ny), uint32_t(nz))];
                }
            }
        }

        // MIC(0) factor, Bridson & Mueller-Fischer 2007 Fig. 4.2. Rows are in x-fastest order, so the
        // -x/-y/-z neighbours always have smaller row indices.
        void BuildPrecon(const std::vector<PressureRow>& rows, std::vector<double>& precon)
        {
            precon.assign(rows.size(), 0.0);
            for (size_t r = 0; r < rows.size(); ++r)
            {
                const PressureRow& row = rows[r];
                double e = row.diag;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const int32_t n = row.nbr[axis * 2];
                    if (n < 0)
                        continue;
                    const PressureRow& nr = rows[size_t(n)];
                    const double p = precon[size_t(n)];
                    // Aplus of the lower neighbour along the other two axes (-1 when linked).
                    double others = 0.0;
                    for (int other = 0; other < 3; ++other)
                    {
                        if (other != axis && nr.nbr[other * 2 + 1] >= 0)
                            others += 1.0;
                    }
                    e -= p * p;
                    e -= MIC_TAU * others * p * p;
                }
                if (e < MIC_SIGMA * row.diag)
                    e = row.diag;
                precon[r] = 1.0 / std::sqrt(e);
            }
        }

        // z = M^-1 r, Fig. 4.3: forward solve into q, then backward solve into z.
        void ApplyPrecon(const std::vector<PressureRow>& rows, const std::vector<double>& precon,
                         const std::vector<double>& r, std::vector<double>& q, std::vector<double>& z)
        {
            const size_t n = rows.size();
            for (size_t i = 0; i < n; ++i)
            {
                const PressureRow& row = rows[i];
                double t = r[i];
                for (int axis = 0; axis < 3; ++axis)
                {
                    const int32_t m = row.nbr[axis * 2];
                    if (m >= 0)
                        t += precon[size_t(m)] * q[size_t(m)];
                }
                q[i] = t * precon[i];
            }
            for (size_t k = n; k-- > 0;)
            {
                const PressureRow& row = rows[k];
                double t = q[k];
                for (int axis = 0; axis < 3; ++axis)
                {
                    const int32_t m = row.nbr[axis * 2 + 1];
                    if (m >= 0)
                        t += precon[k] * z[size_t(m)];
                }
                z[k] = t * precon[k];
            }
        }

        void Multiply(const std::vector<PressureRow>& rows, const std::vector<double>& s, std::vector<double>& out)
        {
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const PressureRow& row = rows[i];
                double t = row.diag * s[i];
                for (int32_t m : row.nbr)
                {
                    if (m >= 0)
                        t -= s[size_t(m)];
                }
                out[i] = t;
            }
        }

        double Dot(const std::vector<double>& a, const std::vector<double>& b)
        {
            double sum = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
                sum += a[i] * b[i];
            return sum;
        }

        double MaxAbs(const std::vector<double>& a)
        {
            double m = 0.0;
            for (double v : a)
                m = (std::max)(m, std::abs(v));
            return m;
        }
    }

    PressureSolveStats SolveUniformPressure(const FlipCellGrid& cells, const std::vector<float>& divergence,
                                            std::vector<float>& pressure, const PressureSolveSettings& settings)
    {
        PressureSolveStats stats;
        pressure.assign(cells.CellCount(), 0.0f);

        const auto buildStart = std::chrono::steady_clock::now();
        std::vector<int32_t> cellToRow;
        std::vector<PressureRow> rows;
        std::vector<size_t> rowCell;
        BuildRows(cells, cellToRow, rows, rowCell);
        std::vector<double> precon;
        BuildPrecon(rows, precon);
        const auto buildEnd = std::chrono::steady_clock::now();
        stats.buildMs = ElapsedMs(buildStart, buildEnd);
        stats.unknowns = uint32_t(rows.size());

        const size_t n = rows.size();
        std::vector<double> b(n);
        for (size_t i = 0; i < n; ++i)
            b[i] = -double(divergence[rowCell[i]]);

        const double bMax = MaxAbs(b);
        if (n == 0 || bMax == 0.0)
        {
            stats.converged = true;
            return stats;
        }

        std::vector<double> p(n, 0.0), r = b, z(n, 0.0), q(n, 0.0), s(n, 0.0), as(n, 0.0);
        ApplyPrecon(rows, precon, r, q, z);
        s = z;
        double sigma = Dot(z, r);
        double rMax = bMax;

        const auto solveStart = std::chrono::steady_clock::now();
        for (uint32_t iter = 0; iter < settings.maxIterations; ++iter)
        {
            Multiply(rows, s, as);
            const double sAs = Dot(s, as);
            if (sAs == 0.0)
                break;
            const double alpha = sigma / sAs;
            for (size_t i = 0; i < n; ++i)
            {
                p[i] += alpha * s[i];
                r[i] -= alpha * as[i];
            }
            stats.iterations = iter + 1;
            rMax = MaxAbs(r);
            if (rMax <= double(settings.tolerance) * bMax)
            {
                stats.converged = true;
                break;
            }
            ApplyPrecon(rows, precon, r, q, z);
            const double sigmaNew = Dot(z, r);
            const double beta = sigma != 0.0 ? sigmaNew / sigma : 0.0;
            for (size_t i = 0; i < n; ++i)
                s[i] = z[i] + beta * s[i];
            sigma = sigmaNew;
        }
        stats.solveMs = ElapsedMs(solveStart, std::chrono::steady_clock::now());
        stats.residual = float(rMax / bMax);

        for (size_t i = 0; i < n; ++i)
            pressure[rowCell[i]] = float(p[i]);
        return stats;
    }
}
