#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // CPU checks of SdfWindField / WindParticleSystem on synthetic grids: far-field velocity equals
    // the wind, numerical divergence is ~0, the normal component vanishes at a wall (inviscid
    // condition, Bridson 2007 sec. 2.4), and particles blown into a floor rarely penetrate it.
    // Returns "OK ..." or "ERR ...".
    std::string RunWindFieldSelfTest();
}

namespace SasamiEngine { using namespace Render; }
