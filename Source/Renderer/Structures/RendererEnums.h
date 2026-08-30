#pragma once

namespace SasamiRenderer
{
    namespace RendererEnums
    {
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

        // Opaque geometry is deferred (OpaqueGBuffer -> Lighting) and transparency is
        // forward (TransparentLighting -> TransparentComposite). The former unlit
        // Opaque/Transparent passes were removed along with the raster-shader-mode
        // switch; their values (1 and 3) stay retired so the numbering keeps meaning.
        enum class RenderPassType
        {
            Shadow = 0,
            Lighting = 2,
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
            ScreenSpaceReflection = 13,
            SoftwareReflection = 14,
            SoftwareReflectionComposite = 15,
            OpaqueGBuffer = 16,
            ScreenSpaceReflectionComposite = 17,
        };
    }
}
