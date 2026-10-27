#pragma once

#include <string>

namespace SasamiEngine::Render::Culling
{
    // Checks HzbPyramid against brute force. Returns "OK checks=N ..." or "ERR <first failure>".
    std::string RunHzbSelfTest();
}
