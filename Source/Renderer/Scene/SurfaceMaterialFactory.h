#pragma once

#include "Renderer/Scene/SurfaceMaterial.h"
#include "Foundation/Math/MathUtil.h"

namespace SasamiEngine::Render
{
    // Untextured opaque metallic-roughness material, as the sample scenes build their primitives.
    inline SurfaceMaterial MakeOpaqueSurfaceMaterial(float r, float g, float b, float roughness, float metallic)
    {
        SurfaceMaterial material{};
        material.baseColor[0] = r;
        material.baseColor[1] = g;
        material.baseColor[2] = b;
        material.baseColor[3] = 1.0f;
        material.roughness = roughness;
        material.metallic = metallic;
        material.reflectionStrength = Math::DefaultReflectionStrength(roughness, metallic);
        material.occlusionStrength = 1.0f;
        return material;
    }
}
