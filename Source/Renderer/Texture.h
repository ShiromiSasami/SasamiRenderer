#pragma once
#include "GraphicsDevice.h"

namespace SasamiRenderer
{
    struct TextureDesc {
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int mips = 1;
        Format format = DXGI_FORMAT_R8G8B8A8_UNORM;
    };

    class Texture {
    public:
        Resource resource;
        GpuDescriptorHandle srv = {};
        TextureDesc desc;
    };
}
