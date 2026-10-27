#pragma once

#include <string>

namespace SasamiEngine::Render::Crowd
{
    // CPU-only checks of BakeVat on a synthetic two-bone rig: frame layout and loop sampling, vertex
    // mode equal to direct AnimationController skinning, bone mode reconstructing vertex mode, bounds,
    // frame-blend wrap, and the input-validation failures. Returns "OK checks=N" or "ERR ...".
    std::string RunVatSelfTest();
}

namespace SasamiEngine { using namespace Render; }
