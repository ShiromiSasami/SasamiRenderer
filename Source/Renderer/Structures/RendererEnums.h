#pragma once

namespace SasamiRenderer
{
    namespace RendererEnums
    {
        enum class RasterShaderMode
        {
            Lighting = 0,
            Opaque = 1,

            // Backward compatibility aliases.
            PBR = Lighting,
            Basic = Opaque,
        };

        enum class GBufferDebugView
        {
            FinalLit = 0,
            Albedo = 1,
            Normal = 2,
            Roughness = 3,
            Metallic = 4,
            AmbientOcclusion = 5,
            Shadow = 6,
            Count
        };

        enum class SkyboxLoadFormat
        {
            Auto = 0,
            HdrEquirect = 1,
            LdrEquirect = 2,
            CubemapFaces = 3,
        };

        enum class RenderNodeType
        {
            Shadow = 0,
            Opaque = 1,
            Lighting = 2,
            Transparent = 3,
            TransparentLighting = 4,
            Skybox = 5,
            PostProcess = 6,
        };
    }
}
