#include "Renderer/Fluid/PbfIsoSurface.h"

#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/ParticleSplatGrid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        double ElapsedMs(std::chrono::steady_clock::time_point begin)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        }
    }

    float SplatLatticeDensity(float spacing, float radius)
    {
        if (spacing <= 0.0f || radius <= 0.0f) {
            return 0.0f;
        }
        const int n = static_cast<int>(std::floor(radius / spacing));
        const double spacing2 = static_cast<double>(spacing) * spacing;
        const double radius2 = static_cast<double>(radius) * radius;
        double sum = 0.0;
        for (int i = -n; i <= n; ++i) {
            for (int j = -n; j <= n; ++j) {
                for (int k = -n; k <= n; ++k) {
                    const double d2 = static_cast<double>(i * i + j * j + k * k) * spacing2;
                    const double s2 = d2 / radius2;
                    if (s2 < 1.0) {
                        const double t = 1.0 - s2;
                        sum += t * t * t;
                    }
                }
            }
        }
        return static_cast<float>(sum);
    }

    void BuildPbfIsoSurface(const std::vector<float>& positions, float particleRadius,
                            const PbfSurfaceSettings& settings, IsoSurfaceMesh& outMesh,
                            PbfSurfaceStats* outStats)
    {
        outMesh.Clear();
        const uint32_t count = static_cast<uint32_t>(positions.size() / 3);
        const float kernelRadius = settings.kernelRadiusScale * particleRadius;
        const float cellSize = settings.cellSizeScale * particleRadius;
        const float latticeDensity = SplatLatticeDensity(2.0f * particleRadius, kernelRadius);
        const float iso = settings.isoFraction * latticeDensity;

        PbfSurfaceStats stats;
        stats.iso = iso;
        stats.latticeDensity = latticeDensity;

        stats.anisotropic = settings.anisotropic;

        if (count > 0 && particleRadius > 0.0f && kernelRadius > 0.0f && cellSize > 0.0f) {
            ParticleSplatGrid grid;
            if (settings.anisotropic) {
                AnisotropicKernelSet kernels;
                ComputeAnisotropicKernels(positions, kernelRadius, 2.0f * particleRadius, settings.anisotropy,
                                          kernels, &stats.anisotropy);
                const auto splatBegin = std::chrono::steady_clock::now();
                const float reach = (std::max)(kernelRadius, stats.anisotropy.maxExtent);
                grid.Fit(kernels.centres.data(), count, 3, reach, cellSize, settings.maxDim);
                grid.SplatAllAnisotropic(kernels.centres.data(), count, kernels.matrices.data(),
                                         kernels.extents.data(), kernels.weights.data());
                stats.splatMs = ElapsedMs(splatBegin);
            } else {
                const auto splatBegin = std::chrono::steady_clock::now();
                grid.Fit(positions.data(), count, 3, kernelRadius, cellSize, settings.maxDim);
                grid.SplatAll(positions.data(), count, 3, nullptr, kernelRadius, nullptr, nullptr);
                stats.splatMs = ElapsedMs(splatBegin);
            }

            const auto meshBegin = std::chrono::steady_clock::now();
            if (grid.CellCount() > 0) {
                ExtractIsoSurface(grid, iso, outMesh);
            }
            stats.meshMs = ElapsedMs(meshBegin);

            const SplatGridDesc& desc = grid.Desc();
            stats.cellSize = desc.cellSize;
            for (int axis = 0; axis < 3; ++axis) {
                stats.dims[axis] = desc.dims[axis];
            }
        }

        if (outStats) {
            *outStats = stats;
        }
    }
}
