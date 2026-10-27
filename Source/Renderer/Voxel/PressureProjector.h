#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class OccupancyBitGrid;
    class StaggeredVelocityGrid;

    struct PressureProjectionSettings
    {
        uint32_t maxIterations = 400;
        // Stop when max |residual| <= tolerance * max |div| of the input (relative).
        float tolerance = 1.0e-5f;
        // MIC(0) tuning constant tau = 0.97 of Bridson & Mueller-Fischer 2007, Fig. 4.2. The 2007 notes have no
        // sigma (only a 1e-30 guard); sigma = 0.25 with "e < sigma*Adiag -> e = Adiag" follows rlguy/GridFluidSim3D
        // and tunabrain/incremental-fluids (said to come from the 2015 book, not checked).
        float micTau = 0.97f;
        float micSigma = 0.25f;
        // Number of z slabs the MIC(0) factor is split into (block Jacobi over slabs, Saad 2003 ch. 12).
        // 1 = the plain sequential MIC(0) of the 2007 notes; more slabs factor and solve in parallel at
        // the cost of a weaker preconditioner (more PCG iterations). The result does not depend on the
        // thread count, only on this value.
        uint32_t micSlabs = 1;
        // The y = 0 face of the domain is a wall (ground). Every other domain face is open.
        bool solidFloor = true;
    };

    struct PressureProjectionStats
    {
        bool valid = false;               // false when the dims of solid and velocity differ
        bool converged = false;
        bool warmStarted = false;         // the pressure passed in was used as the initial guess
        uint32_t fluidCells = 0;
        uint32_t solidCells = 0;          // occupied voxels
        uint32_t enclosedCells = 0;       // empty voxels with no empty path to an open face, treated as solid
        uint32_t iterations = 0;
        uint32_t micSlabs = 1;            // slabs actually used (micSlabs clamped to [1, dimZ])
        float initialMaxDivergence = 0.0f;  // m/s, over fluid cells, after the solid faces were zeroed
        float finalMaxDivergence = 0.0f;    // m/s, recomputed from the corrected velocity
        float solveMs = 0.0f;               // whole ProjectDivergenceFree call
        float preconditionerMs = 0.0f;      // of which the MIC(0) triangular solves (parallel over slabs)
    };

    // Makes `velocity` discretely divergence-free around the occupied voxels of `solid`, one pressure
    // projection with dt / (rho * dx) folded into p (Bridson & Mueller-Fischer 2007 course notes, ch. 4;
    // the PDF did not extract, so the steps below are the standard form, not checked line by line):
    //  1. Cells: solid = occupied; fluid = empty and 6-connected through empty cells to an open domain
    //     face; enclosed = the remaining empty cells (sealed building interiors), treated as solid.
    //  2. Every face touching a solid or enclosed cell gets velocity 0 (static solids, u_solid = 0).
    //     With solidFloor the y = 0 domain faces are 0 as well. Other domain faces are open: a ghost
    //     cell with p = 0 lies outside (Dirichlet), and their velocity is corrected like interior faces.
    //  3. Solve A p = -div over the fluid cells: A_ii = number of non-solid neighbours (fluid cells and
    //     open ghosts), A_ij = -1 for each fluid neighbour j. PCG (Fig. 4.1) with MIC(0) (Figs. 4.2, 4.3).
    //  4. For every face with a fluid cell on at least one side and no solid on either:
    //     u -= p(front) - p(back), with p = 0 in ghosts.
    // A, Dot, and the vector updates run one z slice per job; the MIC(0) factor and triangular solves run one
    // z slab per job (settings.micSlabs, 1 = sequential).
    // inOutPressure (optional, one float per cell): used as the initial guess when its size matches the
    // grid (warm start; ignored on cells outside the solve), and replaced by the solved pressure on return.
    PressureProjectionStats ProjectDivergenceFree(const OccupancyBitGrid& solid, const PressureProjectionSettings& settings,
                                                  StaggeredVelocityGrid& inOutVelocity,
                                                  std::vector<float>* inOutPressure = nullptr);
}

namespace SasamiEngine { using namespace Render; }
