#pragma once
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include <cstdint>

namespace SasamiEngine::Render
{
    using GBufferDebugView         = RendererEnums::GBufferDebugView;
    using RenderPathMode           = RendererEnums::RenderPathMode;
    using RayTracingPerformancePreset = RendererEnums::RayTracingPerformancePreset;
    using AmbientOcclusionMode     = RendererEnums::AmbientOcclusionMode;
    using RuntimeAmbientOcclusionMethod = RendererEnums::RuntimeAmbientOcclusionMethod;
    using PunctualLightFalloff      = RendererEnums::PunctualLightFalloff;

    struct RenderSettings
    {
        float    iblIntensity                                       = 0.0f;
        bool     useTessellation                                    = false;
        bool     tessWireframeEnabled                               = false;
        bool     tessDebugColorsEnabled                             = false;  // flat-shade by per-patch hash color
        // Virtual-geometry cluster path. Default on: measured 13 -> 65 fps on Bistro (Release) with a
        // pixel-identical image at clusterLodThresholdPx = 1. BuildRenderPassExecutionPolicy falls back
        // to the vertex-shader path by itself when the mesh-shader PSOs could not be created.
        bool     useMeshShader                                      = true;
        float    clusterLodThresholdPx                              = 1.0f;  // target screen-space error of the cluster LOD test, pixels
        // Shadow views get their own budget: the unit here is shadow-map texels (the ortho LOD mode
        // feeds projScale = texels per world unit), and shadows are low-frequency and PCF-filtered, so
        // they tolerate far more geometric error than the primary view. Measured on Bistro (Release):
        // Shadow 6.20 -> 3.46 ms at 4 texels, taking the frame from 9.84 ms (102 fps) to ~7.1 ms (~141 fps)
        // while the primary view stays pixel-identical at clusterLodThresholdPx = 1.
        // 2026-09-25: raised 4 -> 8 citing an older "0.00% image difference at 16 texels"
        // measurement. 2026-09-27: REVERTED to 4. A same-camera A/B in Bistro showed the threshold
        // visibly changes the shadow term: at 64 the shadows break into large blocky patches that
        // look like culled cut-outs, and 8 already moves that way. The 0.00% figure came from a
        // viewpoint that did not expose this, and it was taken as sufficient without re-checking.
        // Do not raise this again without an A/B from a viewpoint that shows distant facades.
        float    clusterShadowLodThresholdPx                        = 4.0f;
        // Measurement aid: run the cluster-select compute shader for each cascade but skip the
        // ExecuteIndirect that draws the result. The GPU profiler cannot nest scopes, so the
        // Shadow pass reports selection and drawing as one number; comparing that number with
        // this on and off is the only way to split it. Leaves the shadow map empty, so it is a
        // diagnostic, not a rendering mode.
        bool     clusterShadowSelectOnly                            = false;
        bool     clusterSelectStats                                 = false;
        // Virtual-geometry debug view, main camera only: 0 = off, 1 = colour by cluster,
        // 2 = colour by group (the set a DAG level simplifies together). Replaces the GBuffer
        // albedo only, so lighting still runs and the rest of the GBuffer stays valid.
        uint32_t clusterDebugMode                                   = 0u;
        // Native (non-DX12) directional shadow map. `render.nativeshadow <0|1>` toggles it at
        // runtime, which is how the A/B that validated it was run: shadows change 36% of the
        // pixels in the Bistro arcade and the vaults and soffits darken the way a roofed corridor
        // should. Only DirectX 11 consumes it today.
        bool     nativeShadowEnabled                               = true;
        bool     clusterGpuCulling                                  = true; // GPU BVH traversal + ExecuteIndirect for the cluster path (falls back to the CPU-driven path when unsupported)
        bool     clusterGpuMainView                                 = true; // GPU-driven (ExecuteIndirect) main-view GBuffer cluster draw; needs clusterGpuCulling's subsystems, falls back to the CPU-driven draw
        uint32_t clusterStreamBudgetGroups                          = 0;    // virtual-geometry residency budget in DAG groups; 0 = unlimited (no streaming)
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
        // Measured on Bistro (DX12 raster, SSAO, radius 2.046, exposure at its 1.3 default), by
        // diffing each setting against runtime AO off within one run -- which is bit-exact here,
        // so every number below is signal, not noise. Fraction of the frame that moves by more
        // than 8/255, i.e. visibly:
        //   1.0 -> 14.6%   1.5 -> 19.8%   1.75 -> 23.5%   2.0 -> 25.8%
        //   2.25 -> 27.0%  2.5 -> 27.7%   3.0 -> 28.4%    4.0 -> 29.2%
        // The old default of 1.0 reached only half the occlusion the method can produce, which is
        // why it read as "AO is on but I cannot see it". The curve is ~94% converged at 2.5, and
        // what the remaining headroom buys is depth on pixels that are already dark rather than
        // more occluded area: the worst-pixel darkening is flat from 2.0 to 2.5 (maxAbs 110 ->
        // 120) and then accelerates (3.0 -> 138, 4.0 -> 164), where the foliage interiors and the
        // window reveals visibly crush. 2.5 is the last value before that.
        // Shared with RTAO, where SWRT_AO_CS.hlsl uses it as pow(ao, intensity) rather than as
        // SSAO_PS.hlsl's linear multiplier. Checked there too: 2.5 widens the occluded area
        // (34.8% -> 43.0% of pixels darkened) without deepening the tail (p99.9 drop 56 -> 57),
        // because the exponent leaves ao near 0 and near 1 alone. Re-measure both methods if this
        // value is changed again -- the two mappings do not respond alike.
        float    runtimeAoIntensity                                 = 2.5f;
        float    aoMinOcclusion                                     = 0.1f;  // UE MinOcclusion: 0=full black, >0=min brightness floor
        float    aoDirectLightingStrength                           = 0.5f;  // AO applied to direct lighting (0=indirect only, 1=full, UE-style)
        // Point/spot distance attenuation for every lighting path (raster, DXR, SWRT, GI probes).
        PunctualLightFalloff punctualLightFalloff                   = PunctualLightFalloff::InverseSquare;
        float    runtimeAoThickness                                 = 0.15f;
        uint32_t runtimeAoQuality                                   = 1u;
        uint32_t swrtAoSampleCount                                  = 16u;
        GBufferDebugView gBufferDebugView                          = GBufferDebugView::FinalLit;
        float    hardwareRayTracingResolutionScale                  = 0.75f;
        bool     vsmBlurEnabled                                     = true;
        float    exposure                                           = 1.3f;    // tone map exposure multiplier (ToneMap_PS.hlsl, pre-ACES)
        // HDR10 (PQ / Rec.2020) swap chain output, DX12 raster path only (ToneMap_PS.hlsl,
        // Dx12GraphicsDeviceInit.cpp). Off by default for two reasons: (1) a display that is not
        // in HDR mode -- or a capture/streaming tool reading the backbuffer as SDR -- shows PQ-encoded
        // values as a washed-out, low-contrast image instead of failing loudly, and (2) ImGui draws
        // its overlay directly onto the tone-mapped backbuffer (RendererFrameGraph.cpp's
        // overlay(*cmdList, GetBackBufferRtv(...))) assuming sRGB, so every UI color goes wrong the
        // moment the backbuffer becomes PQ/Rec.2020. Flip on only for deliberate HDR10 testing.
        bool     enableHdrOutput                                    = false;
        // Paper-white luminance in nits used to scale linear scene colour before the PQ encode
        // (ToneMap_PS.hlsl). 200 nits is the commonly used SDR-equivalent paper-white level for
        // HDR10 UI/graphics content (matches the level most HDR game and OS UI overlays target).
        float    hdrPaperWhiteNits                                  = 200.0f;
        bool     fxaaEnabled                                        = true;
        // God rays (LightShaftRenderPass). Off by default: it is an artistic effect, and the
        // pass samples the tone-mapped image rather than a dedicated occlusion mask, so it
        // wants to be turned on deliberately for a scene with visible sky.
        bool     lightShaftEnabled                                  = false;
        float    lightShaftIntensity                                = 0.35f;
        // Bloom (BloomRenderer, Jimenez 2014 mip chain). Off by default so the golden images keep
        // matching. intensity is the ToneMap_PS lerp weight (learnopengl uses 0.04); radius is the
        // upsample tent radius in UV units (learnopengl's filterRadius, 0.005).
        bool     bloomEnabled                                       = false;
        float    bloomIntensity                                     = 0.04f;
        float    bloomRadius                                        = 0.005f;
        // Lens flare (LensFlareRenderPass). Off by default for the same reason as the shafts.
        bool     lensFlareEnabled                                   = false;
        float    lensFlareIntensity                                 = 2.5f;
        int      lensFlareBladeCount                                = 7;
        // 0 = authored sprite table, 1 = Silicon Studio procedural, 2 = ray traced.
        int      lensFlareMode                                      = 1;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
