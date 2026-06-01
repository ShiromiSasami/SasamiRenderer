#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Renderer/Scene/AnimationController.h"
#include "Renderer/Scene/SurfaceMaterial.h"
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
        std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
        bool usesMetallicRoughnessTexture = false;
        SurfaceMaterial material;
        bool transparent = false;
        std::string debugLabel;
        float model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };

    // Proxy for a skinned (bone-animated) mesh. AnimationController is owned by the caller.
    struct SkinnedRenderProxy
    {
        SkinnedMesh                             mesh;
        AnimationController*                    animController = nullptr; // must remain alive
        std::shared_ptr<const CpuTextureRgba8>  albedoTexture;
        std::shared_ptr<const CpuTextureRgba8>  occlusionTexture;
        SurfaceMaterial                         material;
        bool                                    transparent = false;
        float                                   model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
