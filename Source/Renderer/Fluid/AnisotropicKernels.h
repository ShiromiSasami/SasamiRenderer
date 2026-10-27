#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    // Settings of Yu & Turk 2013 ("Reconstructing Surfaces of Particle-Based Fluids Using Anisotropic
    // Kernels", ACM TOG 32(1); sec. 4-5, eq. 6 and 9-16). Defaults are the paper's values except kS.
    struct AnisotropySettings
    {
        float smoothing = 0.9f;        // lambda of eq. 6 (paper: 0.9..1)
        float neighbourScale = 2.0f;   // r_i = neighbourScale * h (paper: 2h)
        float maxRatio = 4.0f;         // k_r: eigenvalues are clamped to >= sigma_1 / k_r (eq. 15)
        float isolatedScale = 0.5f;    // k_n: Sigma~ = k_n I when N <= N_eps
        uint32_t minNeighbours = 25;   // N_eps
        // k_s. The paper's 1400 is tied to its own units; 0 means "derive it": k_s = 1 / sigma, with
        // sigma the covariance eigenvalue of a particle inside an infinite rest lattice of `latticeSpacing`,
        // so an interior particle keeps G = I / h, i.e. the isotropic kernel. (Our choice, not the paper's.)
        float stretchScale = 0.0f;
        // Our deviation: taubinIterations > 0 replaces the single eq. 6 step with that many Taubin 1995
        // lambda|mu pairs (same weights), which smooths without the boundary shrinkage. Pass-band
        // k_PB = 1/lambda + 1/mu = 0.1 with the defaults. 0 = the paper's eq. 6.
        // Our deviation: edgeFade scales eq. 6's lambda per particle by clamp(N_i / N_rest, 0, 1)^edgeFade,
        // N_rest = neighbours of a particle inside the rest lattice, so boundary particles (fewer, one-sided
        // neighbours) move less. 0 = constant lambda (the paper).
        float edgeFade = 0.0f;
        uint32_t taubinIterations = 0;
        float taubinLambda = 0.5f;
        float taubinMu = -0.5263f;
    };

    struct AnisotropyStats
    {
        float stretchScale = 0.0f;     // k_s actually used
        uint32_t isolated = 0;         // particles with N <= N_eps
        uint32_t clamped = 0;          // particles where sigma_1 > k_r * sigma_3
        float maxExtent = 0.0f;        // largest ellipsoid half-extent along any world axis
        double ms = 0.0;
    };

    // Per particle: the smoothed kernel centre (eq. 6) and the symmetric anisotropy matrix
    // G = (1/h) R Sigma~^-1 R^T (eq. 16). A point x is inside particle i's kernel when |G (x - centre)| < 1;
    // an interior particle of a rest lattice gets G ~ I / h. extents[i*3 + a] is the ellipsoid's
    // half-extent along world axis a (row norm of G^-1). det holds det(G) * h^3, the weight that keeps the
    // kernel's integral equal to the isotropic one (eq. 7's det G, relative to I / h).
    struct AnisotropicKernelSet
    {
        std::vector<float> centres;    // xyz per particle
        std::vector<float> matrices;   // 9 per particle, row major, symmetric
        std::vector<float> extents;    // 3 per particle
        std::vector<float> weights;    // det(G) * h^3 per particle
    };

    // positions: xyz packed. h: the kernel radius the isotropic splat would use. latticeSpacing: rest
    // particle spacing (only for the derived k_s). Deterministic (fixed neighbour order, double sums).
    void ComputeAnisotropicKernels(const std::vector<float>& positions, float h, float latticeSpacing,
                                   const AnisotropySettings& settings, AnisotropicKernelSet& out,
                                   AnisotropyStats* outStats = nullptr);

    // Largest covariance eigenvalue (eq. 9-11) of a particle inside an infinite cubic lattice of `spacing`,
    // with neighbours within `neighbourRadius`. The lattice is isotropic, so all three are equal.
    float LatticeCovarianceEigenvalue(float spacing, float neighbourRadius);

    // Neighbours (self excluded) of a particle inside an infinite cubic lattice of `spacing`, within `radius`.
    uint32_t LatticeNeighbourCount(float spacing, float radius);
}

namespace SasamiEngine { using namespace Render; }
