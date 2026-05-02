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
            CookTorranceGGX = Lighting,
            PBR = Lighting,   // deprecated alias
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
            Emissive = 7,
            ScreenSpaceAmbientOcclusionRaw = 8,
            ScreenSpaceAmbientOcclusionFiltered = 9,
            DirectionalLightDirection = 10,
            DirectionalLightNdotL = 11,
            Count
        };

        enum class RenderPathMode
        {
            Raster = 0,
            HardwareRayTracing = 1,
            SdfFluid = 2,
        };

        enum class RayTracingPerformancePreset
        {
            Balanced = 0,
            Performance = 1,
            UltraFast = 2,
        };

        enum class AmbientOcclusionMode
        {
            MaterialOnly = 0,
            SSAOOnly = 1,
            SWRTAOOnly = 2,
            Hybrid = 3,
        };

        enum class RayTracingQualityTier
        {
            Full = 0,
            Fast = 1,
            UltraFast = 2,
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
            SSAO = 7,
            ProceduralSky = 8,
            SdfFluid = 9,
        };
    }
}
