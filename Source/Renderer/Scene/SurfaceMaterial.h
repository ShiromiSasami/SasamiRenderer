#pragma once

#include <cstdint>

namespace SasamiEngine::Render
{
    enum class MaterialWorkflow : uint32_t
    {
        MetallicRoughness = 0,
        SpecularGlossiness = 1,
    };

    // Which lighting model the surface is shaded with. Deliberately a separate axis from
    // MaterialWorkflow: that enum picks how the PBR inputs are authored (metallic-roughness vs
    // specular-glossiness), whereas this picks whether PBR is used at all. Toon replaces the
    // Cook-Torrance evaluation with a banded ramp, so it is not a third "workflow".
    enum class ShadingModel : uint32_t
    {
        Standard = 0,
        Toon = 1,
    };

    // glTF 2.0 alpha coverage (spec 3.9.6 "Alpha Coverage"). Opaque keeps the legacy rule
    // (baseColor alpha < 1 still routes to the transparent pass, which OBJ/FBX rely on).
    // Mask discards texels whose alpha is below alphaCutoff in the GBuffer pass and stays opaque.
    // Blend always routes to the transparent (weighted blended OIT) pass.
    enum class AlphaMode : uint32_t
    {
        Opaque = 0,
        Mask = 1,
        Blend = 2,
    };

    struct SurfaceMaterial
    {
        MaterialWorkflow workflow = MaterialWorkflow::MetallicRoughness;
        // Standard by default, so nothing that does not ask for Toon changes appearance.
        ShadingModel shadingModel = ShadingModel::Standard;
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
        AlphaMode alphaMode = AlphaMode::Opaque;
        float alphaCutoff = 0.5f; // used only when alphaMode == Mask (glTF default 0.5)
        // Toon face: the GBuffer pass replaces N.L with the SDF face-shadow threshold map (t19).
        bool toonFaceShadow = false;
        // Toon hair: the lighting pass replaces the flat highlight with a binarized two-lobe
        // Kajiya-Kay band (angel ring). Carried by the shading selector (0.625), so every GBuffer
        // writer (static, skinned, mesh shader) passes it through unchanged.
        bool toonHair = false;
    };

    // Packs "which workflow" and "which shading model" into the single float that reaches the
    // shader as u_materialSpecularWorkflow.w and is stored in GBufferSpecularWorkflow.a.
    //
    // That GBuffer target is R8G8B8A8_UNORM (RenderTargetPool.cpp), so the channel only carries
    // 0..1 -- a plain enum value would clamp. Three buckets fit comfortably in 8 bits:
    //   0.0 = Standard / metallic-roughness
    //   0.5 = Toon (0.625 = Toon hair; 0.375 = Toon face, written by OpaqueGBuffer_PS only)
    //   1.0 = Standard / specular-glossiness
    // Readers must compare against the midpoints (< 0.25, < 0.75) rather than test equality,
    // because the value survives a UNORM round trip. Keep this in sync with
    // DecodeShadingSelector in PBR_BRDF.hlsli.
    inline float EncodeShadingSelector(const SurfaceMaterial& material)
    {
        if (material.shadingModel == ShadingModel::Toon) {
            return material.toonHair ? 0.625f : 0.5f;
        }
        return (material.workflow == MaterialWorkflow::SpecularGlossiness) ? 1.0f : 0.0f;
    }

    // Value of transparencyParams.z: the GBuffer PS clips alpha below it. 0 disables the test,
    // so only MASK materials pay for it.
    inline float GetGBufferAlphaCutoff(const SurfaceMaterial& material)
    {
        return material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : 0.0f;
    }

    inline bool IsTransparentMaterial(const SurfaceMaterial& material)
    {
        static constexpr float kOpaqueAlphaThreshold = 0.999f;
        static constexpr float kTransparentTransmissionThreshold = 0.01f;
        if (material.alphaMode == AlphaMode::Blend) {
            return true;
        }
        if (material.alphaMode == AlphaMode::Mask) {
            // In MASK mode alpha is a cutoff test, not a blend weight.
            return material.transmission > kTransparentTransmissionThreshold;
        }
        return material.baseColor[3] < kOpaqueAlphaThreshold ||
               material.transmission > kTransparentTransmissionThreshold;
    }
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
