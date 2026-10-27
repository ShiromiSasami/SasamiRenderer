#pragma once
#include "Renderer/RHI/GraphicsDevice.h"

namespace SasamiEngine::Render
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
        RhiTextureHandle rhiTexture{};
        GpuDescriptorHandle srv = {};
        TextureDesc desc;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
