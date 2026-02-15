#pragma once

namespace SasamiRenderer
{
    namespace RendererEnums
    {
        enum class RasterShaderMode
        {
            PBR = 0,
            Basic = 1,
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
    }
}
