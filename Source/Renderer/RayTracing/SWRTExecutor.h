#pragma once
#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderSettings.h"
#include "Renderer/Core/RayTracingStats.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include "Renderer/Scene/LightSystem.h"
#include <cstdint>
#include <limits>

namespace SasamiRenderer
{
    class RenderTargetPool;
    class GpuSoftwareRayTracer;
    class DxrRayTracer;
    class Skybox;
    class IrradianceProbeGrid;

    // Executes software (GPU compute) and hardware ray tracing passes.
    // Owns per-frame caching state (partial update intervals, scene version tracking).
    class SWRTExecutor
    {
    public:
        // Quality/update settings resolved from the current performance preset.
        struct PartialBehavior
        {
            uint32_t shadowMapSize           = 4096u;
            uint32_t shadowUpdateInterval    = 1u;
            float    reflectionResolutionScale = 1.0f;
            uint32_t reflectionUpdateInterval  = 2u;
            uint32_t reflectionPhaseCount      = 2u;
            float    reflectionMaxRoughness    = 0.65f;
            float    reflectionMinEnergy       = 0.005f;
            float    reflectionMaxHitDistance  = 35.0f;
            float    reflectionMaxTraceDistance = 60.0f;
        };

        struct InitParams
        {
            IRHIDevice*          device                = nullptr;
            RenderTargetPool*    renderTargetPool      = nullptr;
            GpuSoftwareRayTracer* gpuSoftwareRayTracer = nullptr;
            DxrRayTracer*        dxrRayTracer          = nullptr;
            RayTracingScene*     rayTracingScene       = nullptr;
            LightSystem*         lightSystem           = nullptr;
            Skybox*              skybox                = nullptr;
            IrradianceProbeGrid* probeGrid             = nullptr;
            DescriptorHeap*      srvHeap               = nullptr;
        };

        // Per-frame camera/viewport data shared across execute calls.
        struct FrameContext
        {
            float cameraPos[3]   = {};
            float cameraInvPV[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            float viewportWidth  = 0.0f;
            float viewportHeight = 0.0f;
            float deltaTime      = 0.0f;
        };

        void Initialize(const InitParams& params);

        // Resolve quality settings for the given preset.
        PartialBehavior ResolveBehavior(RayTracingPerformancePreset preset) const;

        // Accessors for cache state needed by Renderer's pre-pass setup.
        uint32_t GetReflectionPhaseIndex() const { return m_cache.reflectionPhaseIndex; }
        bool IsShadowCacheValid() const { return m_cache.shadowValid; }
        bool IsReflectionCacheValid() const { return m_cache.reflectionValid; }

        // Invalidate from outside (e.g. after scene clear).
        void InvalidateCache();

        // Notify that shadow/reflection GPU resources were reallocated (cache is stale).
        void OnShadowResourcesReallocated()   { m_cache.shadowValid     = false; }
        void OnReflectionResourcesReallocated(){ m_cache.reflectionValid = false; }
        void InvalidateShadowCache()          { m_cache.shadowValid     = false; }

        // Execute software directional shadow (GPU BVH).
        bool ExecuteDirectionalShadow(CommandList* cmdList,
                                      const LightSystem::ShadowPassContext& shadowContext,
                                      const FrameContext& ctx,
                                      const PartialBehavior& behavior,
                                      const RenderSettings& settings,
                                      RayTracingStats& outStats);

        // Execute software reflections (GPU BVH), including optional GI probe update and ReSTIR shadow.
        bool ExecuteReflections(CommandList* cmdList,
                                const FrameContext& ctx,
                                const PartialBehavior& behavior,
                                const RenderSettings& settings,
                                RayTracingStats& outStats);

        bool ExecuteAmbientOcclusion(CommandList* cmdList,
                                     const FrameContext& ctx,
                                     const RenderSettings& settings,
                                     RayTracingStats& outStats);

        // Execute hardware DXR ray tracing.
        bool ExecuteHardware(CommandList* cmdList,
                             UINT backIndex,
                             const FrameContext& ctx,
                             RenderSettings& settings,   // mutable: updates hardwareRayTracingResolutionScale
                             RayTracingStats& outStats);

        bool IsHardwareSupported() const;

    private:
        struct CacheState
        {
            bool     shadowValid                      = false;
            bool     reflectionValid                  = false;
            uint32_t framesSinceShadowUpdate          = 0u;
            uint32_t framesSinceReflectionUpdate      = 0u;
            uint32_t reflectionPhaseIndex             = 0u;
            uint32_t reflectionPendingPhasePasses     = 0u;
            uint32_t shadowMapSize                    = 0u;
            uint32_t reflectionWidth                  = 0u;
            uint32_t reflectionHeight                 = 0u;
            uint64_t shadowSceneVersion               = 0u;
            uint64_t reflectionSceneVersion           = 0u;
            uint64_t shadowLightHash                  = 0u;
            float    shadowLightViewProjection[16]    = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            uint64_t reflectionLightingHash           = 0u;
            float    reflectionCameraPosition[3]      = {};
            float    reflectionInverseViewProjection[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        };

        uint64_t ComputeSceneVersion() const;
        uint64_t ComputeShadowLightHash() const;
        uint64_t ComputeReflectionLightingHash(const RenderSettings& settings) const;
        bool HasShadowProjectionChanged(const float lightViewProjection[16]) const;
        bool HasReflectionCameraChanged(const float cameraPos[3], const float cameraInvPV[16]) const;

        void ConfigureFrameDesc(RayTracingFrameDesc& frameDesc,
                                RayTracingRuntimeStats& outRuntimeStats,
                                const FrameContext& ctx,
                                RenderSettings& settings) const;

        bool ExecuteHardwareImpl(CommandList* cmdList,
                                 UINT backIndex,
                                 const RayTracingFrameDesc& frameDesc,
                                 RayTracingRuntimeStats& outStats);

        IRHIDevice*          m_device              = nullptr;
        RenderTargetPool*    m_renderTargetPool     = nullptr;
        GpuSoftwareRayTracer* m_gpuSoftwareRayTracer = nullptr;
        DxrRayTracer*        m_dxrRayTracer         = nullptr;
        RayTracingScene*     m_rayTracingScene      = nullptr;
        LightSystem*         m_lightSystem          = nullptr;
        Skybox*              m_skybox               = nullptr;
        IrradianceProbeGrid* m_probeGrid            = nullptr;
        DescriptorHeap*      m_srvHeap              = nullptr;

        CacheState m_cache{};
        uint32_t   m_giFrameIndex = 0u;
    };
}
