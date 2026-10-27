#pragma once

#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/Structures/RendererEnums.h"

namespace SasamiEngine::Render
{
    // The seven float4 "extra" slots of the per-draw CameraCB that carry a DrawItem's
    // SurfaceMaterial to the pixel shader (see OpaqueGBuffer_PS.hlsl u_material*).
    struct MaterialConstants
    {
        float extra[7][4] = {};
    };

    // Packs item.material into the same extra0..extra6 layout the OpaqueGBuffer draw loop has
    // always used, so any draw path that shares the CameraCB slot (VS GBuffer, mesh-shader
    // cluster path) reads identical material values.
    MaterialConstants PackMaterialConstants(const SceneSubmitter::DrawItem& item,
                                            RendererEnums::AmbientOcclusionMode effectiveAoMode);
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
