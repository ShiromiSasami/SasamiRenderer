#pragma once

#include <array>

#include "Renderer/Structures/RendererEnums.h"

namespace SasamiRenderer
{
    namespace RenderPassConstants
    {
        using RenderPassType = RendererEnums::RenderPassType;

        // Deferred opaque, then forward transparency:
        //   Shadow -> OpaqueGBuffer -> AO -> Lighting        (opaque, deferred)
        //   -> reflections -> Skybox
        //   -> TransparentBackfaceDistance/SceneColorCopy
        //   -> TransparentLighting -> TransparentComposite   (transparency, forward OIT)
        //   -> PostProcess
        constexpr std::array<RenderPassType, 15> kDefaultRenderPathSequence = {
            RenderPassType::Shadow,
            RenderPassType::OpaqueGBuffer,
            RenderPassType::RuntimeAO,
            RenderPassType::RuntimeAOBlur,
            RenderPassType::Lighting,
            RenderPassType::ScreenSpaceReflection,
            RenderPassType::ScreenSpaceReflectionComposite,
            RenderPassType::SoftwareReflection,
            RenderPassType::SoftwareReflectionComposite,
            RenderPassType::Skybox,
            RenderPassType::TransparentBackfaceDistance,
            RenderPassType::TransparentSceneColorCopy,
            RenderPassType::TransparentLighting,
            RenderPassType::TransparentComposite,
            RenderPassType::PostProcess,
        };
    }
}
