#include "Renderer/Fluid/AnisotropicKernels.h"

#include "Foundation/Jobs/ParallelFor.h"
#include "Foundation/Math/SymmetricEigen3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        constexpr uint32_t PARTICLES_PER_JOB = 256;
        constexpr uint32_t MAX_GRID_DIM = 512;

        // w_ij of eq. 11 from the squared distance; 0 outside the support.
        double NeighbourWeight(double d2, double radius)
        {
            if (d2 >= radius * radius) {
                return 0.0;
            }
            const double s = std::sqrt(d2) / radius;
            return 1.0 - s * s * s;
        }

        // Uniform grid over the particles' AABB with cells >= cellSize; ids sorted by cell, index order within.
        struct NeighbourGrid
        {
            float origin[3] = { 0.0f, 0.0f, 0.0f };
            float cellSize = 1.0f;
            int32_t dims[3] = { 1, 1, 1 };
            std::vector<uint32_t> cellStart;
            std::vector<uint32_t> sortedIds;

            int32_t Coord(float p, int axis) const
            {
                const int32_t c = static_cast<int32_t>(std::floor((p - origin[axis]) / cellSize));
                return std::clamp(c, 0, dims[axis] - 1);
            }

            void Build(const std::vector<float>& positions, uint32_t count, float minCellSize)
            {
                float lo[3] = { positions[0], positions[1], positions[2] };
                float hi[3] = { lo[0], lo[1], lo[2] };
                for (uint32_t i = 1; i < count; ++i) {
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = (std::min)(lo[a], positions[i * 3 + a]);
                        hi[a] = (std::max)(hi[a], positions[i * 3 + a]);
                    }
                }
                cellSize = minCellSize;
                for (int a = 0; a < 3; ++a) {
                    cellSize = (std::max)(cellSize, (hi[a] - lo[a]) / static_cast<float>(MAX_GRID_DIM - 1));
                }
                for (int a = 0; a < 3; ++a) {
                    origin[a] = lo[a];
                    dims[a] = static_cast<int32_t>(std::floor((hi[a] - lo[a]) / cellSize)) + 1;
                }
                const uint32_t cellTotal = static_cast<uint32_t>(dims[0] * dims[1] * dims[2]);
                std::vector<uint32_t> cellOf(count);
                cellStart.assign(cellTotal + 1, 0);
                for (uint32_t i = 0; i < count; ++i) {
                    const int32_t cx = Coord(positions[i * 3], 0);
                    const int32_t cy = Coord(positions[i * 3 + 1], 1);
                    const int32_t cz = Coord(positions[i * 3 + 2], 2);
                    cellOf[i] = static_cast<uint32_t>((cz * dims[1] + cy) * dims[0] + cx);
                    ++cellStart[cellOf[i] + 1];
                }
                for (uint32_t c = 0; c < cellTotal; ++c) {
                    cellStart[c + 1] += cellStart[c];
                }
                std::vector<uint32_t> fill(cellStart.begin(), cellStart.end() - 1);
                sortedIds.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                    sortedIds[fill[cellOf[i]]++] = i;
                }
            }
        };
    }

    namespace
    {
        // fn(j, w_ij) for every particle j (i included) within `radius` of particle i, in a fixed order.
        template <typename Fn>
        void ForEachNeighbour(const NeighbourGrid& grid, const std::vector<float>& positions, uint32_t i,
                              double radius, Fn&& fn)
        {
            const int32_t cx = grid.Coord(positions[i * 3], 0);
            const int32_t cy = grid.Coord(positions[i * 3 + 1], 1);
            const int32_t cz = grid.Coord(positions[i * 3 + 2], 2);
            for (int32_t z = (std::max)(cz - 1, 0); z <= (std::min)(cz + 1, grid.dims[2] - 1); ++z) {
                for (int32_t y = (std::max)(cy - 1, 0); y <= (std::min)(cy + 1, grid.dims[1] - 1); ++y) {
                    for (int32_t x = (std::max)(cx - 1, 0); x <= (std::min)(cx + 1, grid.dims[0] - 1); ++x) {
                        const uint32_t cell = static_cast<uint32_t>((z * grid.dims[1] + y) * grid.dims[0] + x);
                        for (uint32_t k = grid.cellStart[cell]; k < grid.cellStart[cell + 1]; ++k) {
                            const uint32_t j = grid.sortedIds[k];
                            const double d[3] = { double(positions[j * 3]) - positions[i * 3],
                                                  double(positions[j * 3 + 1]) - positions[i * 3 + 1],
                                                  double(positions[j * 3 + 2]) - positions[i * 3 + 2] };
                            const double w = NeighbourWeight(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], radius);
                            if (w > 0.0) {
                                fn(j, w);
                            }
                        }
                    }
                }
            }
        }

        // Taubin 1995 ("A Signal Processing Approach to Fair Surface Design") lambda|mu smoothing of the
        // kernel centres: alternate c += lambda L(c) and c += mu L(c), L(c)_i = sum_j w_ij c_j / sum_j w_ij - c_i,
        // with the eq. 11 weights of the ORIGINAL positions (fixed neighbourhoods). mu < -lambda < 0 undoes the
        // shrinkage a single eq. 6 step leaves at the boundary. Ping-pong buffers, so it is deterministic.
        void TaubinSmoothCentres(const NeighbourGrid& grid, const std::vector<float>& positions, double radius,
                                 uint32_t iterations, double lambda, double mu, std::vector<float>& centres)
        {
            const uint32_t count = static_cast<uint32_t>(positions.size() / 3);
            std::vector<float> next(centres.size());
            const uint32_t jobs = (count + PARTICLES_PER_JOB - 1) / PARTICLES_PER_JOB;
            for (uint32_t step = 0; step < 2 * iterations; ++step) {
                const double factor = (step % 2 == 0) ? lambda : mu;
                Foundation::ParallelFor(jobs, [&](uint32_t job) {
                    const uint32_t first = job * PARTICLES_PER_JOB;
                    const uint32_t last = (std::min)(first + PARTICLES_PER_JOB, count);
                    for (uint32_t i = first; i < last; ++i) {
                        double sumW = 0.0;
                        double mean[3] = { 0.0, 0.0, 0.0 };
                        ForEachNeighbour(grid, positions, i, radius, [&](uint32_t j, double w) {
                            sumW += w;
                            for (int a = 0; a < 3; ++a) {
                                mean[a] += w * centres[j * 3 + a];
                            }
                        });
                        for (int a = 0; a < 3; ++a) {
                            const double c = centres[i * 3 + a];
                            next[i * 3 + a] = static_cast<float>(c + factor * (mean[a] / sumW - c));
                        }
                    }
                });
                centres.swap(next);
            }
        }
    }

    uint32_t LatticeNeighbourCount(float spacing, float radius)
    {
        if (spacing <= 0.0f || radius <= 0.0f) {
            return 0;
        }
        const int n = static_cast<int>(std::floor(radius / spacing));
        uint32_t neighbours = 0;
        for (int i = -n; i <= n; ++i) {
            for (int j = -n; j <= n; ++j) {
                for (int k = -n; k <= n; ++k) {
                    const double d2 = static_cast<double>(i * i + j * j + k * k) * spacing * spacing;
                    if ((i != 0 || j != 0 || k != 0) && NeighbourWeight(d2, radius) > 0.0) {
                        ++neighbours;
                    }
                }
            }
        }
        return neighbours;
    }

    float LatticeCovarianceEigenvalue(float spacing, float neighbourRadius)
    {
        if (spacing <= 0.0f || neighbourRadius <= 0.0f) {
            return 0.0f;
        }
        const int n = static_cast<int>(std::floor(neighbourRadius / spacing));
        double sumW = 0.0;
        double sumWxx = 0.0;
        for (int i = -n; i <= n; ++i) {
            for (int j = -n; j <= n; ++j) {
                for (int k = -n; k <= n; ++k) {
                    const double x = static_cast<double>(i) * spacing;
                    const double d2 = static_cast<double>(i * i + j * j + k * k) * spacing * spacing;
                    const double w = NeighbourWeight(d2, neighbourRadius);
                    sumW += w;
                    sumWxx += w * x * x;
                }
            }
        }
        return sumW > 0.0 ? static_cast<float>(sumWxx / sumW) : 0.0f;
    }

    void ComputeAnisotropicKernels(const std::vector<float>& positions, float h, float latticeSpacing,
                                   const AnisotropySettings& settings, AnisotropicKernelSet& out,
                                   AnisotropyStats* outStats)
    {
        const auto begin = std::chrono::steady_clock::now();
        const uint32_t count = static_cast<uint32_t>(positions.size() / 3);
        out.centres.assign(size_t(count) * 3, 0.0f);
        out.matrices.assign(size_t(count) * 9, 0.0f);
        out.extents.assign(size_t(count) * 3, 0.0f);
        out.weights.assign(count, 0.0f);

        AnisotropyStats stats;
        const double radius = static_cast<double>(settings.neighbourScale) * h;
        float stretchScale = settings.stretchScale;
        if (stretchScale <= 0.0f) {
            const float sigma = LatticeCovarianceEigenvalue(latticeSpacing, static_cast<float>(radius));
            stretchScale = sigma > 0.0f ? 1.0f / sigma : 1.0f;
        }
        stats.stretchScale = stretchScale;

        if (count == 0 || !(h > 0.0f) || !(radius > 0.0)) {
            if (outStats) {
                *outStats = stats;
            }
            return;
        }

        NeighbourGrid grid;
        grid.Build(positions, count, static_cast<float>(radius));

        std::vector<uint8_t> isolated(count, 0);
        std::vector<uint8_t> clamped(count, 0);
        // With Taubin smoothing the eq. 6 step is replaced, not stacked.
        const double lambda = settings.taubinIterations > 0 ? 0.0 : settings.smoothing;
        const double restNeighbours = (std::max)(1u, LatticeNeighbourCount(latticeSpacing, static_cast<float>(radius)));
        const double edgeFade = settings.edgeFade;
        const double ks = stretchScale;
        const double kr = (std::max)(settings.maxRatio, 1.0f);
        const double kn = settings.isolatedScale;
        const double hd = h;

        const uint32_t jobs = (count + PARTICLES_PER_JOB - 1) / PARTICLES_PER_JOB;
        Foundation::ParallelFor(jobs, [&](uint32_t job) {
            const uint32_t first = job * PARTICLES_PER_JOB;
            const uint32_t last = (std::min)(first + PARTICLES_PER_JOB, count);
            for (uint32_t i = first; i < last; ++i) {
                const double xi[3] = { positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2] };
                const int32_t cx = grid.Coord(positions[i * 3], 0);
                const int32_t cy = grid.Coord(positions[i * 3 + 1], 1);
                const int32_t cz = grid.Coord(positions[i * 3 + 2], 2);

                // Pass 1: weighted mean (eq. 10) and neighbour count.
                double sumW = 0.0;
                double mean[3] = { 0.0, 0.0, 0.0 };
                uint32_t neighbours = 0;
                const auto forEachNeighbour = [&](auto&& fn) {
                    for (int32_t z = (std::max)(cz - 1, 0); z <= (std::min)(cz + 1, grid.dims[2] - 1); ++z) {
                        for (int32_t y = (std::max)(cy - 1, 0); y <= (std::min)(cy + 1, grid.dims[1] - 1); ++y) {
                            for (int32_t x = (std::max)(cx - 1, 0); x <= (std::min)(cx + 1, grid.dims[0] - 1); ++x) {
                                const uint32_t cell = static_cast<uint32_t>((z * grid.dims[1] + y) * grid.dims[0] + x);
                                for (uint32_t k = grid.cellStart[cell]; k < grid.cellStart[cell + 1]; ++k) {
                                    const uint32_t j = grid.sortedIds[k];
                                    const double d[3] = { positions[j * 3] - xi[0], positions[j * 3 + 1] - xi[1],
                                                          positions[j * 3 + 2] - xi[2] };
                                    const double w = NeighbourWeight(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], radius);
                                    if (w > 0.0) {
                                        fn(j, w);
                                    }
                                }
                            }
                        }
                    }
                };
                forEachNeighbour([&](uint32_t j, double w) {
                    sumW += w;
                    for (int a = 0; a < 3; ++a) {
                        mean[a] += w * positions[j * 3 + a];
                    }
                    if (j != i) {
                        ++neighbours;
                    }
                });
                double lambdaI = lambda;
                if (edgeFade > 0.0) {
                    lambdaI *= std::pow((std::min)(1.0, static_cast<double>(neighbours) / restNeighbours), edgeFade);
                }
                for (int a = 0; a < 3; ++a) {
                    mean[a] /= sumW; // sumW >= 1: the particle itself has w = 1
                    out.centres[i * 3 + a] = static_cast<float>((1.0 - lambdaI) * xi[a] + lambdaI * mean[a]); // eq. 6
                }

                // Sigma~ eigenvalues and axes.
                double sigma[3] = { kn, kn, kn };
                double R[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
                if (neighbours > settings.minNeighbours) {
                    // Pass 2: weighted covariance (eq. 9).
                    double c[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
                    forEachNeighbour([&](uint32_t j, double w) {
                        const double d[3] = { positions[j * 3] - mean[0], positions[j * 3 + 1] - mean[1],
                                              positions[j * 3 + 2] - mean[2] };
                        c[0] += w * d[0] * d[0];
                        c[1] += w * d[0] * d[1];
                        c[2] += w * d[0] * d[2];
                        c[3] += w * d[1] * d[1];
                        c[4] += w * d[1] * d[2];
                        c[5] += w * d[2] * d[2];
                    });
                    for (double& v : c) {
                        v /= sumW;
                    }
                    double values[3];
                    Foundation::SymmetricEigen3(c, values, R); // eq. 12-13
                    const double floorValue = values[0] / kr;  // eq. 15
                    if (values[2] < floorValue) {
                        clamped[i] = 1;
                    }
                    for (int k = 0; k < 3; ++k) {
                        sigma[k] = ks * (std::max)(values[k], floorValue);
                    }
                } else {
                    isolated[i] = 1;
                }

                // G = (1/h) R Sigma~^-1 R^T (eq. 16); G^-1 = h R Sigma~ R^T for the extents.
                float* g = &out.matrices[size_t(i) * 9];
                for (int r = 0; r < 3; ++r) {
                    double extent2 = 0.0;
                    for (int col = 0; col < 3; ++col) {
                        double gValue = 0.0;
                        double inverse = 0.0;
                        for (int k = 0; k < 3; ++k) {
                            gValue += R[r][k] * R[col][k] / sigma[k];
                            inverse += R[r][k] * R[col][k] * sigma[k];
                        }
                        g[r * 3 + col] = static_cast<float>(gValue / hd);
                        inverse *= hd;
                        extent2 += inverse * inverse;
                    }
                    out.extents[i * 3 + r] = static_cast<float>(std::sqrt(extent2));
                }
                out.weights[i] = static_cast<float>(1.0 / (sigma[0] * sigma[1] * sigma[2]));
            }
        });

        if (settings.taubinIterations > 0) {
            TaubinSmoothCentres(grid, positions, radius, settings.taubinIterations, settings.taubinLambda,
                                settings.taubinMu, out.centres);
        }

        for (uint32_t i = 0; i < count; ++i) {
            stats.isolated += isolated[i];
            stats.clamped += clamped[i];
            for (int a = 0; a < 3; ++a) {
                stats.maxExtent = (std::max)(stats.maxExtent, out.extents[i * 3 + a]);
            }
        }
        stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        if (outStats) {
            *outStats = stats;
        }
    }
}
