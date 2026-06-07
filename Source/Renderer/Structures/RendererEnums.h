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
            RuntimeAmbientOcclusionRaw = 8,
            RuntimeAmbientOcclusionFiltered = 9,
            DirectionalLightDirection = 10,
            DirectionalLightNdotL = 11,
            ReflectionRadiance = 12,
            ReflectionAlpha = 13,
            SwrtReflectionHitDistance = 14,
            SwrtReflectionComposite = 15,
            Count,

            // Backward compatibility aliases.
            ScreenSpaceAmbientOcclusionRaw = RuntimeAmbientOcclusionRaw,
            ScreenSpaceAmbientOcclusionFiltered = RuntimeAmbientOcclusionFiltered,
        };

        enum class RenderPathMode
        {
            Raster = 0,
            HardwareRayTracing = 1,
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
            RuntimeAOOnly = 1,
            RayTracedAOOnly = 2,
            Hybrid = 3,

            // Backward compatibility aliases.
            SSAOOnly = RuntimeAOOnly,
            SWRTAOOnly = RayTracedAOOnly,
        };

        enum class RuntimeAmbientOcclusionMethod
        {
            SSAO = 0,
            RayTraced = 1,
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

        enum class RenderPassType
        {
            Shadow = 0,
            Opaque = 1,
            Lighting = 2,
            Transparent = 3,
            TransparentLighting = 4,
            Skybox = 5,
            PostProcess = 6,
            RuntimeAO = 7,
            SSAO = RuntimeAO, // backward compatibility alias
            ProceduralSky = 8,
            TransparentBackfaceDistance = 9,
            TransparentComposite = 10,
            RuntimeAOBlur = 11,
            TransparentSceneColorCopy = 12,
            SoftwareReflection = 13,
            SoftwareReflectionComposite = 14,
        };
    }
}
