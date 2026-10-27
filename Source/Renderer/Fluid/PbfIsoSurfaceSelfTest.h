#pragma once

#include <string>

namespace SasamiEngine::Render::Fluid
{
    // CPU checks of BuildPbfIsoSurface: SplatLatticeDensity matches a brute-force lattice sum, a rest block
    // of N^3 lattice particles meshes to one closed surface (CountOpenEdges 0) whose volume is the block's
    // within 10 %, 0 particles give an empty mesh, two runs are bit-identical, and a short PbfSolver dam
    // break still meshes closed with positive volume. Returns "OK checks=N ..." or "ERR ...".
    std::string RunPbfIsoSurfaceSelfTest();
}

namespace SasamiEngine { using namespace Render; }
