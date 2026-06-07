#pragma once

#include "Renderer/Config/RenderSettings.h"

namespace SasamiRenderer
{
    struct RenderFeatureSettingChanges
    {
        bool invalidateShadowCache = false;
        bool reallocateReflectionResources = false;
        bool invalidateSceneColorHistory = false;
    };

    class RenderFeatureSettings : public RenderSettings
    {
    public:
        void SetIblIntensity(float intensity)
        {
            iblIntensity = (intensity < 0.0f) ? 0.0f : intensity;
        }

        void SetRayTracingMaxBounceCount(uint32_t count)
        {
            if (count < kMinRayTracingBounceCount) {
                count = kMinRayTracingBounceCount;
            }
            if (count > kMaxRayTracingBounceCount) {
                count = kMaxRayTracingBounceCount;
            }
            rayTracingMaxBounceCount = count;
        }

        RenderFeatureSettingChanges SetRasterSoftwareRayTracedDirectionalShadowEnabled(bool enabled)
        {
            RenderFeatureSettingChanges changes{};
            if (rasterSoftwareRayTracedDirectionalShadowEnabled != enabled) {
                changes.invalidateShadowCache = true;
            }
            rasterSoftwareRayTracedDirectionalShadowEnabled = enabled;
            return changes;
        }

        RenderFeatureSettingChanges SetRasterSoftwareRayTracedReflectionEnabled(bool enabled)
        {
            RenderFeatureSettingChanges changes{};
            if (enabled && rasterScreenSpaceReflectionEnabled) {
                rasterScreenSpaceReflectionEnabled = false;
                changes.invalidateSceneColorHistory = true;
            }
            if (rasterSoftwareRayTracedReflectionEnabled != enabled) {
                changes.reallocateReflectionResources = true;
            }
            rasterSoftwareRayTracedReflectionEnabled = enabled;
            return changes;
        }

        RenderFeatureSettingChanges SetRasterScreenSpaceReflectionEnabled(bool enabled)
        {
            RenderFeatureSettingChanges changes{};
            if (enabled && rasterSoftwareRayTracedReflectionEnabled) {
                rasterSoftwareRayTracedReflectionEnabled = false;
                changes.reallocateReflectionResources = true;
            }
            if (rasterScreenSpaceReflectionEnabled != enabled) {
                changes.invalidateSceneColorHistory = true;
            }
            rasterScreenSpaceReflectionEnabled = enabled;
            return changes;
        }

        bool IsRasterSoftwareRayTracedAmbientOcclusionEnabled() const
        {
            return ambientOcclusionMode != RendererEnums::AmbientOcclusionMode::MaterialOnly &&
                   (runtimeAoMethod == RuntimeAmbientOcclusionMethod::RayTraced ||
                    ambientOcclusionMode == RendererEnums::AmbientOcclusionMode::RayTracedAOOnly);
        }

        void SetRasterSoftwareRayTracedAmbientOcclusionEnabled(bool enabled)
        {
            rasterSoftwareRayTracedAmbientOcclusionEnabled = enabled;
            if (enabled) {
                ambientOcclusionMode = RendererEnums::AmbientOcclusionMode::RuntimeAOOnly;
                runtimeAoMethod = RuntimeAmbientOcclusionMethod::RayTraced;
            } else {
                ambientOcclusionMode = RendererEnums::AmbientOcclusionMode::Hybrid;
                runtimeAoMethod = RuntimeAmbientOcclusionMethod::SSAO;
            }
        }

        void SetAmbientOcclusionMode(RendererEnums::AmbientOcclusionMode mode)
        {
            ambientOcclusionMode = mode;
            if (mode == RendererEnums::AmbientOcclusionMode::RayTracedAOOnly) {
                runtimeAoMethod = RuntimeAmbientOcclusionMethod::RayTraced;
            }
            rasterSoftwareRayTracedAmbientOcclusionEnabled = IsRasterSoftwareRayTracedAmbientOcclusionEnabled();
        }

        void SetRuntimeAmbientOcclusionMethod(RuntimeAmbientOcclusionMethod method)
        {
            runtimeAoMethod = method;
            rasterSoftwareRayTracedAmbientOcclusionEnabled =
                (method == RuntimeAmbientOcclusionMethod::RayTraced);
        }

        void SetGBufferDebugView(GBufferDebugView view)
        {
            if (renderPathMode != RenderPathMode::Raster) {
                gBufferDebugView = GBufferDebugView::FinalLit;
                return;
            }

            const int index = static_cast<int>(view);
            const int count = static_cast<int>(GBufferDebugView::Count);
            gBufferDebugView = (index >= 0 && index < count) ? view : GBufferDebugView::FinalLit;
        }

        void CycleGBufferDebugView(int delta)
        {
            if (renderPathMode != RenderPathMode::Raster) {
                gBufferDebugView = GBufferDebugView::FinalLit;
                return;
            }

            const int count = static_cast<int>(GBufferDebugView::Count);
            if (count <= 0) {
                gBufferDebugView = GBufferDebugView::FinalLit;
                return;
            }

            int index = static_cast<int>(gBufferDebugView);
            index = (index + delta) % count;
            if (index < 0) {
                index += count;
            }
            gBufferDebugView = static_cast<GBufferDebugView>(index);
        }

        void SetRuntimeAORadius(float r) { runtimeAoRadius = (r > 0.0f) ? r : 0.01f; }
        void SetRuntimeAOIntensity(float i) { runtimeAoIntensity = (i >= 0.0f) ? i : 0.0f; }
        void SetRuntimeAOThickness(float t) { runtimeAoThickness = (t >= 0.0f) ? t : 0.0f; }
        void SetRuntimeAOQuality(uint32_t q) { runtimeAoQuality = (q > 2u) ? 2u : q; }
        void SetSwrtSamplingMode(uint32_t mode) { swrtSamplingMode = (mode < 2u ? mode : 2u); }
        void SetSwrtMaxBounces(uint32_t n) { swrtMaxBounces = (n < 1u ? 1u : (n > 8u ? 8u : n)); }
        void SetSwrtReflectionAtrousIterations(uint32_t n) { swrtReflectionAtrousIterations = (n > 5u ? 5u : n); }

        void SetSwrtAoSampleCount(uint32_t count)
        {
            if (count < 4u) {
                count = 4u;
            } else if (count > 32u) {
                count = 32u;
            }
            swrtAoSampleCount = count;
        }

        void SetAoMinOcclusion(float v)
        {
            if (v < 0.0f) {
                v = 0.0f;
            } else if (v > 1.0f) {
                v = 1.0f;
            }
            aoMinOcclusion = v;
        }
    };
}
