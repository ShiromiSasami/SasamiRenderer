#pragma once

#include <cstdint>

namespace SasamiRenderer
{
    enum class MaterialWorkflow : uint32_t
    {
        MetallicRoughness = 0,
        SpecularGlossiness = 1,
    };

    struct SurfaceMaterial
    {
        MaterialWorkflow workflow = MaterialWorkflow::MetallicRoughness;
        float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float specularColor[3] = { 0.04f, 0.04f, 0.04f };
        float emissive[3] = { 0.0f, 0.0f, 0.0f };
        float roughness = 0.5f;
        float metallic = 0.0f;
        float reflectionStrength = 0.0f;
        float occlusionStrength = 1.0f;
        float transmission = 0.0f;
        float ior = 1.5f;
        float transparentShellStrength = 1.0f;
        float thickness = 0.0f;
        float attenuationColor[3] = { 1.0f, 1.0f, 1.0f };
        float attenuationDistance = 1.0f;
        // Thin geometry (awnings, foliage, curtains) opts in so back faces flip their
        // normal instead of shading black; front faces are unaffected either way.
        bool doubleSided = false;
    };

    inline bool IsTransparentMaterial(const SurfaceMaterial& material)
    {
        static constexpr float kOpaqueAlphaThreshold = 0.999f;
        static constexpr float kTransparentTransmissionThreshold = 0.01f;
        return material.baseColor[3] < kOpaqueAlphaThreshold ||
               material.transmission > kTransparentTransmissionThreshold;
    }
}
