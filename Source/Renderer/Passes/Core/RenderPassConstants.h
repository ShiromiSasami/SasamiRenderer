#pragma once

#include <array>

#include "Renderer/Structures/RendererEnums.h"

namespace SasamiRenderer
{
    namespace RenderPassConstants
    {
        using RenderPassType = RendererEnums::RenderPassType;

        // Default forward render path order.
        constexpr std::array<RenderPassType, 14> kDefaultRenderPathSequence = {
            RenderPassType::Shadow,
            RenderPassType::Opaque,
            RenderPassType::RuntimeAO,
            RenderPassType::RuntimeAOBlur,
            RenderPassType::Lighting,
            RenderPassType::SoftwareReflection,
            RenderPassType::SoftwareReflectionComposite,
            RenderPassType::Skybox,
            RenderPassType::TransparentBackfaceDistance,
            RenderPassType::TransparentSceneColorCopy,
            RenderPassType::Transparent,
            RenderPassType::TransparentLighting,
            RenderPassType::TransparentComposite,
            RenderPassType::PostProcess,
        };
    }
}
