#pragma once

#include <array>

#include "Renderer/Structures/RendererEnums.h"

namespace SasamiRenderer
{
    namespace RenderNodeConstants
    {
        using RenderNodeType = RendererEnums::RenderNodeType;

        // Default forward render path order.
        constexpr std::array<RenderNodeType, 10> kDefaultRenderPathSequence = {
            RenderNodeType::Shadow,
            RenderNodeType::Opaque,
            RenderNodeType::RuntimeAO,
            RenderNodeType::Lighting,
            RenderNodeType::Skybox,
            RenderNodeType::TransparentBackfaceDistance,
            RenderNodeType::Transparent,
            RenderNodeType::TransparentLighting,
            RenderNodeType::TransparentComposite,
            RenderNodeType::PostProcess,
        };
    }
}
