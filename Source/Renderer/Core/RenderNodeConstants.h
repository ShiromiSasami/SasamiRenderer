#pragma once

#include <array>

#include "Renderer/Structures/RendererEnums.h"

namespace SasamiRenderer
{
    namespace RenderNodeConstants
    {
        using RenderNodeType = RendererEnums::RenderNodeType;

        // Default forward render path order.
        constexpr std::array<RenderNodeType, 8> kDefaultRenderPathSequence = {
            RenderNodeType::Shadow,
            RenderNodeType::Opaque,
            RenderNodeType::RuntimeAO,
            RenderNodeType::Lighting,
            RenderNodeType::Skybox,
            RenderNodeType::Transparent,
            RenderNodeType::TransparentLighting,
            RenderNodeType::PostProcess,
        };
    }
}
