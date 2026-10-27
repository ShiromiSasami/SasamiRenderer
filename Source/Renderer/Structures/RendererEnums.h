#pragma once

namespace SasamiEngine::Render
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
            // Flat colour per selected CSM cascade (red/green/blue/yellow for 0..3,
            // magenta past the last cascade). Diagnostic for close-range shadow instability.
            ShadowCascadeIndex = 16,
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

        // Which technique supplies each half of the indirect lighting. They are separate
        // enums because they are separate terms: the probe grid stores L2 SH, which fits a
        // cosine lobe (diffuse) and cannot represent a sharp reflection, while the reflection
        // techniques are view-dependent and per-pixel. An enum rather than a set of bools so
        // "two specular sources at once" is not a representable state.
        //
        // Not every value is implemented yet -- see Renderer::IsIndirectSpecularSourceAvailable
        // and IsIndirectDiffuseSourceAvailable, which is what the UI greys out. HardwareRayTracing
        // in particular is currently a whole-pipeline mode (RenderPassRegistry swaps the pass list
        // for the DXR pass, which writes all of SceneColor), not a per-term contributor.
        enum class IndirectSpecularSource
        {
            None = 0,
            ScreenSpace = 1,
            SoftwareRayTracing = 2,
            HardwareRayTracing = 3,
        };

        enum class IndirectDiffuseSource
        {
            None = 0,
            ProbeGI = 1,
            SoftwareRayTracing = 2,
            HardwareRayTracing = 3,
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

        // Point/spot distance attenuation, shared by every lighting path. Values match
        // PUNCTUAL_FALLOFF_* in Shaders/Shared/Common/PunctualLightFalloff.hlsli.
        enum class PunctualLightFalloff
        {
            LinearSquared = 0,  // saturate(1 - d/r)^2, the legacy raster/DXR curve
            SmoothWindow = 1,   // saturate(1 - (d/r)^2)^2, the legacy SWRT/GI curve
            InverseSquare = 2,  // windowed 1/d^2 (Karis 2013 eq. 9, Filament)
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
            ToonOutline = 18,
        };
    }
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
