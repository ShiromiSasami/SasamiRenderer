#pragma once

#include <string>

namespace SasamiEngine::Render::Fluid
{
    // Poly6 normalization, grid vs brute force neighbors, dam break sanity and determinism.
    // Returns "OK checks=N ..." or "ERR <first failure>".
    std::string RunPbfSelfTest();
}
