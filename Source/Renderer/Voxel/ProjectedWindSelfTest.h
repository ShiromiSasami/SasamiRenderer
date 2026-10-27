#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // CPU checks of StaggeredVelocityGrid / ProjectDivergenceFree / ProjectedWindField on synthetic grids:
    // an empty domain keeps the uniform wind, a block obstacle leaves max |div| ~ 0 with zero flux through
    // its faces, the wind slows in front of the block and speeds up over it, a sealed hollow box is
    // classified as enclosed and stays still, and particles rarely enter the block,
    // also with curl-noise gusts added (GustedWindField), whose part stays nearly divergence-free.
    // Returns "OK ..." or "ERR ...".
    std::string RunProjectedWindSelfTest();
}

namespace SasamiEngine { using namespace Render; }
