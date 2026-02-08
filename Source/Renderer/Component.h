#pragma once
#include <cstddef>
#include "Texture.h"

namespace SasamiRenderer
{
    struct Component {
        virtual ~Component() = default;
    };

    struct ModelComponent : public Component {
        size_t meshIndex = 0;
    };

    struct TextureComponent : public Component {
        Texture* texture = nullptr;
    };
}

