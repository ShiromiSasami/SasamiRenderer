#pragma once
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include <cstdint>

namespace SasamiRenderer
{
    using RasterShaderMode         = RendererEnums::RasterShaderMode;
    using GBufferDebugView         = RendererEnums::GBufferDebugView;
    using RenderPathMode           = RendererEnums::RenderPathMode;
    using RayTracingPerformancePreset = RendererEnums::RayTracingPerformancePreset;
    using AmbientOcclusionMode     = RendererEnums::AmbientOcclusionMode;
    using RuntimeAmbientOcclusionMethod = RendererEnums::RuntimeAmbientOcclusionMethod;

    struct RenderSettings
    {
        float    iblIntensity                                       = 0.25f;
        bool     useTessellation                                    = false;
        bool     tessWireframeEnabled                               = false;
        bool     tessDebugColorsEnabled                             = false;  // flat-shade by per-patch hash color
        bool     meshletDebugViewEnabled                            = false;
        bool     useMeshShader                                      = true;  // default: Mesh Shader path
        RasterShaderMode rasterShaderMode                          = RasterShaderMode::Lighting;
        RenderPathMode   renderPathMode                            = RenderPathMode::Raster;
        RayTracingPerformancePreset rayTracingPerformancePreset     = RayTracingPerformancePreset::Balanced;
        bool     rayTracingDynamicResolutionEnabled                 = true;
        uint32_t rayTracingMaxBounceCount                          = kDefaultRayTracingBounceCount;
        bool     rasterSoftwareRayTracedDirectionalShadowEnabled    = false;
        bool     rasterSoftwareRayTracedReflectionEnabled           = false;
        bool     rasterScreenSpaceReflectionEnabled                 = false;
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
        float    runtimeAoThickness                                 = 0.15f;
        uint32_t runtimeAoQuality                                   = 1u;
        uint32_t swrtAoSampleCount                                  = 16u;
        GBufferDebugView gBufferDebugView                          = GBufferDebugView::FinalLit;
        float    hardwareRayTracingResolutionScale                  = 0.75f;
        bool     vsmBlurEnabled                                     = true;
    };
}
