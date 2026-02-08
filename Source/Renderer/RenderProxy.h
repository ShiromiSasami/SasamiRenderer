#pragma once

#include <string>

#include "Mesh.h"

namespace SasamiRenderer
{
    struct RenderProxy
    {
        Mesh mesh;
        std::wstring texturePath;
        float model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
