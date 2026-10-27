#pragma once

#include "Renderer/Fluid/AnisotropicKernels.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    struct IsoSurfaceMesh;

    // Surfacing settings for PBF particles, in units of the solver's particleRadius r (rest spacing 2r).
    struct PbfSurfaceSettings
    {
        float kernelRadiusScale = 4.0f;  // splat radius R = scale * r (4r = the solver's default h)
        float cellSizeScale = 1.0f;      // grid cell = scale * r (half the rest spacing)
        // iso = isoFraction * density of a cell centre inside an infinite rest lattice
        // (SplatLatticeDensity), so 0.5 puts a flat rest face about where the lattice cells end.
        float isoFraction = 0.5f;
        uint32_t maxDim = 160;           // per-axis cap passed to ParticleSplatGrid::Fit
        // Yu & Turk 2013 anisotropic kernels (smoothed centres + per-particle ellipsoids) instead of spheres.
        bool anisotropic = false;
        AnisotropySettings anisotropy;
    };

    struct PbfSurfaceStats
    {
        float iso = 0.0f;
        float latticeDensity = 0.0f;
        float cellSize = 0.0f;
        uint32_t dims[3] = { 0, 0, 0 };
        double splatMs = 0.0;
        double meshMs = 0.0;
        bool anisotropic = false;
        AnisotropyStats anisotropy;      // only when anisotropic
    };

    // Sum of the splat kernel k(d / radius) = max(0, 1 - (d / radius)^2)^3 (ParticleSplatGrid) over every
    // point of an infinite cubic lattice of `spacing` around one lattice point, the point itself included.
    float SplatLatticeDensity(float spacing, float radius);

    // Particle surfacing (Zhu & Bridson 2005, sec. 6) of PBF particles: fits a ParticleSplatGrid around
    // them, splats every particle with weight 1 and radius kernelRadiusScale * particleRadius, and extracts
    // the iso level with ExtractIsoSurface (marching tetrahedra; closed, outward normals). With
    // settings.anisotropic the splat uses ComputeAnisotropicKernels (h = kernel radius) at the same iso.
    // positions: xyz packed (PbfSolver::Positions()). An empty set gives an empty mesh. Deterministic.
    void BuildPbfIsoSurface(const std::vector<float>& positions, float particleRadius,
                            const PbfSurfaceSettings& settings, IsoSurfaceMesh& outMesh,
                            PbfSurfaceStats* outStats = nullptr);
}

namespace SasamiEngine { using namespace Render; }
