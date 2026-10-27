#pragma once

#include <string>

namespace SasamiEngine::Render::ParticleCharacter
{
    // CPU-only checks of SurfaceSampleSet and MolecularBody on synthetic meshes: barycentric range,
    // determinism per seed, area weighting (3:1 triangles), interpolation landing on the triangle,
    // a bonded body staying on static targets, release + re-form converging, floor never crossed,
    // serial vs parallel bit identity. Returns "OK checks=N ..." or "ERR ...".
    std::string RunParticleCharacterSelfTest();
}

namespace SasamiEngine { using namespace Render; }
