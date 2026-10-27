#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // CPU checks for WindDrivenParticles and WindFireSpread on synthetic fields (no scene needed):
    // petal terminal velocity in still air (|g| * tau), drift with a uniform wind, smoke rising,
    // wall push-out, and fire on a flat floor spreading farther downwind than upwind (and evenly
    // with no wind). Returns "OK ..." or "ERR ...".
    std::string RunWindEffectsSelfTest();
}
