#pragma once

#include "Renderer/RHI/RhiDevice.h"

#include <string>

namespace SasamiEngine::Render::Culling
{
    // Checks GpuHzbBuilder against HzbPyramid, texel by texel, bit for bit.
    // Returns "OK ..." or "ERR <first failure>". Blocks until the GPU is idle.
    std::string RunGpuHzbSelfTest(IRhiDevice& device);
}
