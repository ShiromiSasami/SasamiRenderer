#pragma once

#include <string>

namespace SasamiEngine::Render::Toon
{
    // Synthetic checks of BuildFaceShadowThresholdMap. Returns "OK checks=N ..." or "ERR ...".
    std::string RunFaceShadowSelfTest();
}

namespace SasamiEngine { using namespace Render; }
