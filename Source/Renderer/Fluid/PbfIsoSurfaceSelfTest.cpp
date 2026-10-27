#include "Renderer/Fluid/PbfIsoSurfaceSelfTest.h"

#include "Foundation/Math/SymmetricEigen3.h"
#include "Renderer/Fluid/AnisotropicKernels.h"
#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/PbfIsoSurface.h"
#include "Renderer/Fluid/PbfSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        std::string Fail(const char* what, const std::ostringstream& detail)
        {
            return std::string("ERR ") + what + " " + detail.str();
        }

        // Independent lattice sum with an explicit distance test.
        double BruteForceLatticeDensity(double spacing, double radius)
        {
            double sum = 0.0;
            for (int i = -5; i <= 5; ++i) {
                for (int j = -5; j <= 5; ++j) {
                    for (int k = -5; k <= 5; ++k) {
                        const double d = std::sqrt(static_cast<double>(i * i + j * j + k * k)) * spacing;
                        if (d < radius) {
                            const double s = d / radius;
                            sum += std::pow(1.0 - s * s, 3.0);
                        }
                    }
                }
            }
            return sum;
        }

        std::vector<float> MakeRestBlock(uint32_t n, float spacing, const float (&origin)[3])
        {
            std::vector<float> positions;
            positions.reserve(size_t(n) * n * n * 3);
            for (uint32_t z = 0; z < n; ++z) {
                for (uint32_t y = 0; y < n; ++y) {
                    for (uint32_t x = 0; x < n; ++x) {
                        positions.push_back(origin[0] + (static_cast<float>(x) + 0.5f) * spacing);
                        positions.push_back(origin[1] + (static_cast<float>(y) + 0.5f) * spacing);
                        positions.push_back(origin[2] + (static_cast<float>(z) + 0.5f) * spacing);
                    }
                }
            }
            return positions;
        }
    }

    std::string RunPbfIsoSurfaceSelfTest()
    {
        uint32_t checks = 0;
        std::ostringstream detail;
        detail << std::fixed << std::setprecision(5);
        const PbfSurfaceSettings settings;

        // 1. SplatLatticeDensity against a brute force sum.
        {
            const float lattice = SplatLatticeDensity(0.05f, 0.1f);
            const double expected = BruteForceLatticeDensity(0.05, 0.1);
            const float centreOnly = SplatLatticeDensity(0.05f, 0.01f);
            detail << "lattice=" << lattice << " expected=" << expected << " centreOnly=" << centreOnly;
            ++checks;
            if (!(std::fabs(lattice - expected) <= 1e-4 * expected)) {
                return Fail("lattice density", detail);
            }
            ++checks;
            if (!(std::fabs(centreOnly - 1.0f) <= 1e-6f)) {
                return Fail("lattice density centre only", detail);
            }
        }

        // 2. Empty input.
        {
            IsoSurfaceMesh mesh;
            BuildPbfIsoSurface(std::vector<float>(), 0.025f, settings, mesh);
            ++checks;
            if (mesh.TriangleCount() != 0) {
                detail.str("");
                detail << "triangles=" << mesh.TriangleCount();
                return Fail("empty input is not empty", detail);
            }
        }

        // 3. Rest block meshes to one closed surface of about the block's volume.
        constexpr uint32_t N = 10;
        constexpr float RADIUS = 0.025f;
        constexpr float SPACING = 0.05f;
        const float origin[3] = { 0.3f, 0.1f, -0.2f };
        const std::vector<float> block = MakeRestBlock(N, SPACING, origin);
        std::string blockSummary;
        IsoSurfaceMesh blockMesh;
        {
            PbfSurfaceStats stats;
            BuildPbfIsoSurface(block, RADIUS, settings, blockMesh, &stats);
            const uint32_t openEdges = CountOpenEdges(blockMesh);
            const float volume = IsoSurfaceVolume(blockMesh);
            const double expectedVolume = std::pow(static_cast<double>(N) * SPACING, 3.0);
            detail.str("");
            detail << "openEdges=" << openEdges << " V=" << volume << " expectedV=" << expectedVolume
                   << " triangles=" << blockMesh.TriangleCount()
                   << " dims=" << stats.dims[0] << "x" << stats.dims[1] << "x" << stats.dims[2];
            ++checks;
            if (blockMesh.TriangleCount() == 0 || openEdges != 0) {
                return Fail("block mesh not closed", detail);
            }
            ++checks;
            if (!(volume > 0.0f) || !(std::fabs(volume - expectedVolume) <= 0.1 * expectedVolume)) {
                return Fail("block volume", detail);
            }
            blockSummary = detail.str();
        }

        // 4. Determinism.
        {
            IsoSurfaceMesh again;
            BuildPbfIsoSurface(block, RADIUS, settings, again);
            const bool samePositions = again.positions.size() == blockMesh.positions.size()
                && std::memcmp(again.positions.data(), blockMesh.positions.data(),
                               again.positions.size() * sizeof(float)) == 0;
            ++checks;
            if (!samePositions || again.indices != blockMesh.indices) {
                detail.str("");
                detail << "vertices=" << again.VertexCount() << "/" << blockMesh.VertexCount();
                return Fail("block mesh not deterministic", detail);
            }
        }

        // 5. Dam break.
        std::string damSummary;
        {
            PbfParams params;
            PbfSolver solver;
            solver.Reset(params);
            const float lo[3] = { 0.0f, 0.0f, 0.0f };
            const float hi[3] = { 0.4f, 0.4f, 0.4f };
            solver.SpawnBlock(lo, hi);
            for (uint32_t step = 0; step < 60; ++step) {
                solver.Step(1.0f / 120.0f);
            }
            IsoSurfaceMesh mesh;
            PbfSurfaceStats stats;
            BuildPbfIsoSurface(solver.Positions(), params.particleRadius, settings, mesh, &stats);
            const uint32_t openEdges = CountOpenEdges(mesh);
            const float volume = IsoSurfaceVolume(mesh);
            std::ostringstream summary;
            summary << std::fixed << std::setprecision(4) << "particles=" << solver.ParticleCount()
                    << " triangles=" << mesh.TriangleCount() << " V=" << volume
                    << " splatMs=" << stats.splatMs << " meshMs=" << stats.meshMs;
            detail.str("");
            detail << "openEdges=" << openEdges << " " << summary.str();
            ++checks;
            if (mesh.TriangleCount() == 0 || openEdges != 0 || !(volume > 0.0f)) {
                return Fail("dam break mesh", detail);
            }
            damSummary = summary.str();
        }

        // 6. SymmetricEigen3: A = R diag R^T, R orthonormal, values descending.
        {
            const double a[6] = { 4.0, 1.0, -2.0, 3.0, 0.5, 1.5 };
            double values[3];
            double R[3][3];
            Foundation::SymmetricEigen3(a, values, R);
            const double full[3][3] = { { a[0], a[1], a[2] }, { a[1], a[3], a[4] }, { a[2], a[4], a[5] } };
            double maxErr = 0.0;
            double maxOrtho = 0.0;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    double rebuilt = 0.0;
                    double dot = 0.0;
                    for (int k = 0; k < 3; ++k) {
                        rebuilt += R[r][k] * values[k] * R[c][k];
                        dot += R[k][r] * R[k][c];
                    }
                    maxErr = (std::max)(maxErr, std::fabs(rebuilt - full[r][c]));
                    maxOrtho = (std::max)(maxOrtho, std::fabs(dot - (r == c ? 1.0 : 0.0)));
                }
            }
            detail.str("");
            detail << std::scientific << "eigen=" << values[0] << "," << values[1] << "," << values[2]
                   << " rebuildErr=" << maxErr << " orthoErr=" << maxOrtho;
            ++checks;
            if (!(maxErr < 1e-10 && maxOrtho < 1e-12 && values[0] >= values[1] && values[1] >= values[2])) {
                return Fail("symmetric eigen", detail);
            }
        }

        // 7. Anisotropic kernels: a particle deep inside the rest block keeps the isotropic kernel
        //    (G = I / h, weight 1) because k_s is derived from the same lattice.
        PbfSurfaceSettings anisoSettings = settings;
        anisoSettings.anisotropic = true;
        const float kernelRadius = settings.kernelRadiusScale * RADIUS;
        {
            constexpr uint32_t M = 14;
            const std::vector<float> big = MakeRestBlock(M, SPACING, origin);
            AnisotropicKernelSet kernels;
            AnisotropyStats stats;
            ComputeAnisotropicKernels(big, kernelRadius, SPACING, anisoSettings.anisotropy, kernels, &stats);
            const uint32_t centre = ((M / 2) * M + M / 2) * M + M / 2;
            double maxGErr = 0.0;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    const double expected = (r == c) ? 1.0 / kernelRadius : 0.0;
                    maxGErr = (std::max)(maxGErr, std::fabs(kernels.matrices[size_t(centre) * 9 + r * 3 + c] - expected)
                                                      * kernelRadius);
                }
            }
            double maxCentreShift = 0.0;
            for (int a = 0; a < 3; ++a) {
                maxCentreShift = (std::max)(maxCentreShift, static_cast<double>(std::fabs(
                    kernels.centres[size_t(centre) * 3 + a] - big[size_t(centre) * 3 + a])));
            }
            detail.str("");
            detail << "interiorGErr=" << maxGErr << " weight=" << kernels.weights[centre]
                   << " centreShift=" << maxCentreShift << " ks=" << stats.stretchScale
                   << " isolated=" << stats.isolated << " clamped=" << stats.clamped;
            ++checks;
            if (!(maxGErr < 1e-3 && std::fabs(kernels.weights[centre] - 1.0f) < 1e-3f && maxCentreShift < 1e-5)) {
                return Fail("anisotropic interior", detail);
            }
        }

        // 8. Anisotropic rest block: closed, volume within 10%, deterministic; area against the isotropic one.
        std::string anisoSummary;
        {
            IsoSurfaceMesh mesh;
            PbfSurfaceStats stats;
            BuildPbfIsoSurface(block, RADIUS, anisoSettings, mesh, &stats);
            IsoSurfaceMesh again;
            BuildPbfIsoSurface(block, RADIUS, anisoSettings, again);
            const uint32_t openEdges = CountOpenEdges(mesh);
            const float volume = IsoSurfaceVolume(mesh);
            const double expectedVolume = std::pow(static_cast<double>(N) * SPACING, 3.0);
            std::ostringstream summary;
            summary << std::fixed << std::setprecision(4) << "openEdges=" << openEdges << " V=" << volume
                    << " A=" << IsoSurfaceArea(mesh) << " isoA=" << IsoSurfaceArea(blockMesh)
                    << " triangles=" << mesh.TriangleCount() << " clamped=" << stats.anisotropy.clamped
                    << " isolated=" << stats.anisotropy.isolated << " maxExtent=" << stats.anisotropy.maxExtent
                    << " anisoMs=" << stats.anisotropy.ms;
            detail.str("");
            detail << summary.str();
            ++checks;
            if (mesh.TriangleCount() == 0 || openEdges != 0) {
                return Fail("aniso block mesh not closed", detail);
            }
            // Yu & Turk sec. 4.2: the eq. 6 smoothing pulls boundary kernels inward, so the volume shrinks
            // (bounded by a constant times the particle radius, their appendix A). On a 10^3 block that is a
            // large fraction, so the check bounds the shrink per face instead: at most 1.5 rest spacings,
            // and no growth beyond the isotropic tolerance.
            PbfSurfaceSettings unsmoothed = anisoSettings;
            unsmoothed.anisotropy.smoothing = 0.0f;
            IsoSurfaceMesh unsmoothedMesh;
            BuildPbfIsoSurface(block, RADIUS, unsmoothed, unsmoothedMesh);
            const float unsmoothedVolume = IsoSurfaceVolume(unsmoothedMesh);
            const double side = static_cast<double>(N) * SPACING;
            const double minVolume = std::pow(side - 2.0 * 1.5 * SPACING, 3.0);
            detail << " unsmoothedV=" << unsmoothedVolume << " minV=" << minVolume;
            summary << " lambda0V=" << unsmoothedVolume;
            ++checks;
            if (!(volume >= minVolume) || !(volume <= 1.1 * expectedVolume)
                || CountOpenEdges(unsmoothedMesh) != 0 || !(unsmoothedVolume <= 1.1 * expectedVolume)) {
                return Fail("aniso block volume", detail);
            }
            const bool same = again.positions.size() == mesh.positions.size()
                && std::memcmp(again.positions.data(), mesh.positions.data(), mesh.positions.size() * sizeof(float)) == 0
                && again.indices == mesh.indices;
            ++checks;
            if (!same) {
                return Fail("aniso block not deterministic", detail);
            }
            anisoSummary = summary.str();
        }

        std::ostringstream out;
        out << "OK checks=" << checks << " block[" << blockSummary << "] dam[" << damSummary << "] aniso["
            << anisoSummary << "]";
        return out.str();
    }
}
