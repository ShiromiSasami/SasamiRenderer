#pragma once

#include <string>

namespace SasamiEngine::Render::Crowd
{
    // CPU-only checks of the crowd store and update: serial vs chunked parallel bit identity,
    // bounds reflection, animation-time wrap, spawn determinism. Returns "OK checks=N" or "ERR ...".
    std::string RunCrowdSelfTest();
}

namespace SasamiEngine { using namespace Render; }
