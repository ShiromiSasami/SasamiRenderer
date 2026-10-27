#include "Renderer/Scene/MaterialConstants.h"

namespace SasamiEngine::Render
{
    MaterialConstants PackMaterialConstants(const SceneSubmitter::DrawItem& item,
                                            RendererEnums::AmbientOcclusionMode effectiveAoMode)
    {
        MaterialConstants mc{};

        mc.extra[0][0] = item.material.baseColor[0];
        mc.extra[0][1] = item.material.baseColor[1];
        mc.extra[0][2] = item.material.baseColor[2];
        mc.extra[0][3] = item.material.baseColor[3];

        mc.extra[1][0] = item.material.emissive[0];
        mc.extra[1][1] = item.material.emissive[1];
        mc.extra[1][2] = item.material.emissive[2];
        mc.extra[1][3] = item.material.roughness;

        mc.extra[2][0] = item.material.metallic;
        mc.extra[2][1] = item.material.occlusionStrength;
        mc.extra[2][2] = static_cast<float>(effectiveAoMode);
        mc.extra[2][3] = item.usesMetallicRoughnessTexture ? 1.0f : 0.0f;

        mc.extra[3][0] = item.material.specularColor[0];
        mc.extra[3][1] = item.material.specularColor[1];
        mc.extra[3][2] = item.material.specularColor[2];
        mc.extra[3][3] = EncodeShadingSelector(item.material);

        mc.extra[4][0] = item.material.reflectionStrength;
        mc.extra[4][1] = item.material.transmission;
        mc.extra[4][2] = item.material.ior;
        mc.extra[4][3] = item.material.thickness;

        mc.extra[5][0] = item.material.attenuationColor[0];
        mc.extra[5][1] = item.material.attenuationColor[1];
        mc.extra[5][2] = item.material.attenuationColor[2];
        mc.extra[5][3] = item.material.attenuationDistance;

        mc.extra[6][0] = item.material.transparentShellStrength;
        // y: double-sided flag. OpaqueGBuffer_PS negates the normal on back
        // faces only when this is set, so single-sided materials keep their
        // existing shading exactly.
        mc.extra[6][1] = item.material.doubleSided ? 1.0f : 0.0f;
        mc.extra[6][2] = GetGBufferAlphaCutoff(item.material); // z: MASK alpha cutoff
        mc.extra[6][3] = item.material.toonFaceShadow ? 1.0f : 0.0f; // w: SDF face-shadow flag

        return mc;
    }
}
