#pragma once
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include <cstdint>

namespace SasamiRenderer
{
    using GBufferDebugView         = RendererEnums::GBufferDebugView;
    using RenderPathMode           = RendererEnums::RenderPathMode;
    using RayTracingPerformancePreset = RendererEnums::RayTracingPerformancePreset;
    using AmbientOcclusionMode     = RendererEnums::AmbientOcclusionMode;
    using RuntimeAmbientOcclusionMethod = RendererEnums::RuntimeAmbientOcclusionMethod;

    struct RenderSettings
    {
        float    iblIntensity                                       = 0.0f;
        bool     useTessellation                                    = false;
        bool     tessWireframeEnabled                               = false;
        bool     tessDebugColorsEnabled                             = false;  // flat-shade by per-patch hash color
        bool     meshletDebugViewEnabled                            = false;
        bool     useMeshShader                                      = true;  // default: Mesh Shader path
        RenderPathMode   renderPathMode                            = RenderPathMode::Raster;
        RayTracingPerformancePreset rayTracingPerformancePreset     = RayTracingPerformancePreset::Balanced;
        bool     rayTracingDynamicResolutionEnabled                 = true;
        uint32_t rayTracingMaxBounceCount                          = kDefaultRayTracingBounceCount;
        bool     rasterSoftwareRayTracedDirectionalShadowEnabled    = false;
        bool     rasterSoftwareRayTracedReflectionEnabled           = false;
        bool     rasterScreenSpaceReflectionEnabled                 = false;
        float    ssrMaxDistance                                     = 28.0f;   // world-space ray march distance
        float    ssrThickness                                       = 0.18f;   // view-space depth thickness test
        float    ssrStepCount                                       = 48.0f;   // max ray-march steps
        float    ssrRoughnessCutoff                                 = 0.78f;   // roughness above which SSR fully fades out
        float    ssrRefineSteps                                     = 4.0f;    // binary refinement steps
        float    ssrEdgeFade                                        = 0.075f;  // screen edge fade width
        float    ssrNormalOffset                                    = 0.02f;   // ray origin normal bias
        float    ssrIntensity                                       = 1.0f;    // reflection intensity multiplier
        bool     rasterSoftwareRayTracedAmbientOcclusionEnabled     = false;
        AmbientOcclusionMode ambientOcclusionMode                  = AmbientOcclusionMode::Hybrid;
        RuntimeAmbientOcclusionMethod runtimeAoMethod              = RuntimeAmbientOcclusionMethod::SSAO;
        bool     swrtUseReSTIR                                      = true;
        uint32_t swrtSamplingMode                                   = 2u;
        uint32_t swrtSamplesPerPixel                                = 2u;
        uint32_t swrtMaxBounces                                     = 2u;
        bool     swrtDenoiserEnabled                                = true;
        float    swrtReflectionTemporalAlpha                        = 0.1f;
        uint32_t swrtReflectionAtrousIterations                     = 3u;
        float    swrtReflectionAtrousPhiDepth                       = 0.35f;
        bool     volumetricCloudEnabled                             = false;
        float    cloudCover                                         = 0.45f;
        float    cloudDensity                                       = 2.0f;
        float    cloudWindSpeed                                     = 8.0f;
        float    cloudBaseAlt                                       = 1500.0f;
        float    cloudTopAlt                                        = 5000.0f;
        bool     runtimeAoEnabled                                   = true;
        float    runtimeAoRadius                                    = 0.5f;
        float    runtimeAoBias                                      = 0.025f;
        float    runtimeAoIntensity                                 = 1.0f;
        float    aoMinOcclusion                                     = 0.1f;  // UE MinOcclusion: 0=full black, >0=min brightness floor
        float    aoDirectLightingStrength                           = 0.5f;  // AO applied to direct lighting (0=indirect only, 1=full, UE-style)
        float    runtimeAoThickness                                 = 0.15f;
        uint32_t runtimeAoQuality                                   = 1u;
        uint32_t swrtAoSampleCount                                  = 16u;
        GBufferDebugView gBufferDebugView                          = GBufferDebugView::FinalLit;
        float    hardwareRayTracingResolutionScale                  = 0.75f;
        bool     vsmBlurEnabled                                     = true;
        float    exposure                                           = 1.3f;    // tone map exposure multiplier (ToneMap_PS.hlsl, pre-ACES)
        bool     fxaaEnabled                                        = true;
    };
}
