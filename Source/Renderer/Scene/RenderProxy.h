#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Renderer/Structures/Mesh.h"

namespace SasamiRenderer
{
    struct CpuTextureRgba8
    {
        uint64_t id = 0;
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct RenderProxy
    {
        Mesh mesh;
        std::shared_ptr<const CpuTextureRgba8> albedoTexture;
        float model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
