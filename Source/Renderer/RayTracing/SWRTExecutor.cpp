#define NOMINMAX
#include "Renderer/RayTracing/SWRTExecutor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Resources/RenderTargetPool.h"
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"
#include "Renderer/RayTracing/DxrRayTracer.h"
#include "Renderer/Scene/Skybox.h"
#include "Renderer/GI/IrradianceProbeGrid.h"
#include "d3dx12.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace SasamiRenderer
{
    namespace
    {
        constexpr float kTargetRayTracingFrameMs = 1000.0f / 64.0f;

        bool UsesReflectionDebugView(RendererEnums::GBufferDebugView view)
        {
            return view == RendererEnums::GBufferDebugView::ReflectionRadiance ||
                   view == RendererEnums::GBufferDebugView::ReflectionAlpha ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionHitDistance ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionComposite;
        }

        void HashBytes(uint64_t& hash, const void* data, size_t size)
        {
            static constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
            static constexpr uint64_t kFnvPrime       = 1099511628211ull;
            if (hash == 0ull) {
                hash = kFnvOffsetBasis;
            }
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                hash ^= static_cast<uint64_t>(bytes[i]);
                hash *= kFnvPrime;
            }
        }

        float ClampRayTracingResolutionScale(RayTracingPerformancePreset preset, float scale)
        {
            const float minScale = (preset == RayTracingPerformancePreset::UltraFast) ? 0.25f : 0.35f;
            const float maxScale = (preset == RayTracingPerformancePreset::Balanced)  ? 1.0f  : 0.85f;
            return std::clamp(scale, minScale, maxScale);
        }

        uint32_t ComputeScaledDimension(uint32_t dimension, float scale)
        {
            return std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(dimension) * scale)));
        }

        using RayTracingQualityTier = RendererEnums::RayTracingQualityTier;

        RayTracingQualityTier ResolveRayTracingQualityTier(RayTracingPerformancePreset preset,
                                                           float scale,
                                                           float frameMs)
        {
            if (preset == RayTracingPerformancePreset::UltraFast ||
                scale <= 0.40f ||
                frameMs > kTargetRayTracingFrameMs * 1.55f) {
                return RayTracingQualityTier::UltraFast;
            }
            if (preset == RayTracingPerformancePreset::Performance ||
                scale <= 0.62f ||
                frameMs > kTargetRayTracingFrameMs * 1.15f) {
                return RayTracingQualityTier::Fast;
            }
            return RayTracingQualityTier::Full;
        }

        uint32_t ResolvePointLightBudget(RayTracingQualityTier qualityTier)
        {
            switch (qualityTier) {
            case RayTracingQualityTier::UltraFast: return 1u;
            case RayTracingQualityTier::Fast:      return 4u;
            case RayTracingQualityTier::Full:
            default:                               return 8u;
            }
        }

        uint32_t ResolveSpotLightBudget(RayTracingQualityTier qualityTier)
        {
            switch (qualityTier) {
            case RayTracingQualityTier::UltraFast: return 0u;
            case RayTracingQualityTier::Fast:      return 2u;
            case RayTracingQualityTier::Full:
            default:                               return 4u;
            }
        }
    }

    void SWRTExecutor::Initialize(const InitParams& params)
    {
        m_device               = params.device;
        m_renderTargetPool     = params.renderTargetPool;
        m_gpuSoftwareRayTracer = params.gpuSoftwareRayTracer;
        m_dxrRayTracer         = params.dxrRayTracer;
        m_rayTracingScene      = params.rayTracingScene;
        m_lightSystem          = params.lightSystem;
        m_skybox               = params.skybox;
        m_probeGrid            = params.probeGrid;
        m_srvHeap              = params.srvHeap;
    }

    SWRTExecutor::PartialBehavior SWRTExecutor::ResolveBehavior(RayTracingPerformancePreset preset) const
    {
        PartialBehavior b{};
        switch (preset) {
        case RayTracingPerformancePreset::UltraFast:
            b.shadowMapSize            = 128u;
            b.shadowUpdateInterval     = 3u;
            b.reflectionResolutionScale  = 1.0f;
            b.reflectionUpdateInterval   = 4u;
            b.reflectionPhaseCount       = 4u;
            b.reflectionMaxRoughness     = 0.55f;
            b.reflectionMinEnergy        = 0.015f;
            b.reflectionMaxHitDistance   = 18.0f;
            b.reflectionMaxTraceDistance = 30.0f;
            break;
        case RayTracingPerformancePreset::Performance:
            b.shadowMapSize            = 256u;
            b.shadowUpdateInterval     = 2u;
            b.reflectionResolutionScale  = 1.0f;
            b.reflectionUpdateInterval   = 3u;
            b.reflectionPhaseCount       = 2u;
            b.reflectionMaxRoughness     = 0.65f;
            b.reflectionMinEnergy        = 0.01f;
            b.reflectionMaxHitDistance   = 25.0f;
            b.reflectionMaxTraceDistance = 45.0f;
            break;
        case RayTracingPerformancePreset::Balanced:
        default:
            // defaults already set
            break;
        }
        return b;
    }

    void SWRTExecutor::InvalidateCache()
    {
        m_cache.shadowValid                  = false;
        m_cache.reflectionValid              = false;
        m_cache.framesSinceShadowUpdate      = 0u;
        m_cache.framesSinceReflectionUpdate  = 0u;
        m_cache.reflectionPhaseIndex         = 0u;
        m_cache.reflectionPendingPhasePasses = 0u;
    }

    bool SWRTExecutor::IsHardwareSupported() const
    {
        return m_dxrRayTracer && m_dxrRayTracer->IsSupported();
    }

    uint64_t SWRTExecutor::ComputeSceneVersion() const
    {
        uint64_t hash = 0ull;
        HashBytes(hash, &m_rayTracingScene->geometryVersion, sizeof(m_rayTracingScene->geometryVersion));
        HashBytes(hash, &m_rayTracingScene->materialVersion, sizeof(m_rayTracingScene->materialVersion));
        HashBytes(hash, &m_rayTracingScene->instanceVersion, sizeof(m_rayTracingScene->instanceVersion));
        return hash;
    }

    uint64_t SWRTExecutor::ComputeShadowLightHash() const
    {
        uint64_t hash = 0ull;
        const auto settings = m_lightSystem->GetDirectionalLightSettings();
        HashBytes(hash, &settings, sizeof(settings));
        return hash;
    }

    uint64_t SWRTExecutor::ComputeReflectionLightingHash(const RenderSettings& settings) const
    {
        uint64_t hash = 0ull;
        const auto dirLight = m_lightSystem->GetDirectionalLightSettings();
        HashBytes(hash, &dirLight, sizeof(dirLight));

        const auto& pointLights = m_lightSystem->GetPointLights();
        const uint64_t pointCount = static_cast<uint64_t>(pointLights.size());
        HashBytes(hash, &pointCount, sizeof(pointCount));
        if (!pointLights.empty()) {
            HashBytes(hash, pointLights.data(), pointLights.size() * sizeof(pointLights[0]));
        }

        const auto& spotLights = m_lightSystem->GetSpotLights();
        const uint64_t spotCount = static_cast<uint64_t>(spotLights.size());
        HashBytes(hash, &spotCount, sizeof(spotCount));
        if (!spotLights.empty()) {
            HashBytes(hash, spotLights.data(), spotLights.size() * sizeof(spotLights[0]));
        }

        const bool iblEnabled       = m_skybox->IsIblEnabled();
        const float iblPrefilterMaxMip = m_skybox->GetIblPrefilterMaxMip();
        HashBytes(hash, &iblEnabled,       sizeof(iblEnabled));
        HashBytes(hash, &settings.iblIntensity, sizeof(settings.iblIntensity));
        HashBytes(hash, &iblPrefilterMaxMip, sizeof(iblPrefilterMaxMip));
        HashBytes(hash, &settings.gBufferDebugView, sizeof(settings.gBufferDebugView));
        return hash;
    }

    bool SWRTExecutor::HasShadowProjectionChanged(const float lightViewProjection[16]) const
    {
        const float matrixThreshold = 1e-4f;
        for (size_t i = 0; i < 16; ++i) {
            if (std::fabs(lightViewProjection[i] - m_cache.shadowLightViewProjection[i]) > matrixThreshold) {
                return true;
            }
        }
        return false;
    }

    bool SWRTExecutor::HasReflectionCameraChanged(const float cameraPos[3],
                                                   const float cameraInvPV[16]) const
    {
        // Reflection history can tolerate small camera jitter. If we invalidate on
        // every tiny shake, the low-res / low-spp current frame gets exposed and
        // produces black flicker on glossy surfaces.
        const float positionThresholdSq = 2.5e-3f; // ~5 cm
        const float matrixThreshold     = 1e-3f;

        const float dx = cameraPos[0] - m_cache.reflectionCameraPosition[0];
        const float dy = cameraPos[1] - m_cache.reflectionCameraPosition[1];
        const float dz = cameraPos[2] - m_cache.reflectionCameraPosition[2];
        if (dx * dx + dy * dy + dz * dz > positionThresholdSq) {
            return true;
        }
        for (size_t i = 0; i < 16; ++i) {
            if (std::fabs(cameraInvPV[i] - m_cache.reflectionInverseViewProjection[i]) > matrixThreshold) {
                return true;
            }
        }
        return false;
    }

    void SWRTExecutor::ConfigureFrameDesc(RayTracingFrameDesc& frameDesc,
                                          RayTracingRuntimeStats& outRuntimeStats,
                                          const FrameContext& ctx,
                                          RenderSettings& settings) const
    {
        frameDesc.width  = static_cast<UINT>(ctx.viewportWidth);
        frameDesc.height = static_cast<UINT>(ctx.viewportHeight);
        frameDesc.renderWidth  = frameDesc.width;
        frameDesc.renderHeight = frameDesc.height;
        std::memcpy(frameDesc.inverseViewProjection, ctx.cameraInvPV, sizeof(ctx.cameraInvPV));
        std::memcpy(frameDesc.cameraPosition, ctx.cameraPos, sizeof(ctx.cameraPos));
        frameDesc.directionalLight             = m_lightSystem->GetDirectionalLightSettings();
        frameDesc.pointLights                  = &m_lightSystem->GetPointLights();
        frameDesc.spotLights                   = &m_lightSystem->GetSpotLights();
        frameDesc.debugView                    = static_cast<uint32_t>(settings.gBufferDebugView);
        frameDesc.iblEnabled                   = m_skybox->IsIblEnabled();
        frameDesc.iblIntensity                 = settings.iblIntensity;
        frameDesc.iblPrefilterMaxMip           = m_skybox->GetIblPrefilterMaxMip();
        frameDesc.directionalLightMarkerEnabled         = m_skybox->IsDirectionalLightMarkerEnabled();
        frameDesc.directionalLightMarkerAngularRadius   = m_skybox->GetDirectionalLightMarkerAngularRadius();
        frameDesc.directionalLightMarkerHaloAngularRadius = m_skybox->GetDirectionalLightMarkerHaloAngularRadius();
        frameDesc.directionalLightMarkerBrightness      = m_skybox->GetDirectionalLightMarkerBrightness();
        frameDesc.diffuseShCoefficients  = m_skybox->HasDiffuseShCoefficients()
            ? m_skybox->GetDiffuseShCoefficients() : nullptr;
        frameDesc.prefilterSubresources  = m_skybox->HasCpuPrefilterData()
            ? &m_skybox->GetCpuPrefilterSubresources() : nullptr;
        frameDesc.prefilterBaseSize      = m_skybox->GetCpuPrefilterBaseSize();
        frameDesc.prefilterMipLevels     = m_skybox->GetCpuPrefilterMipLevels();
        frameDesc.brdfLutPixels          = m_skybox->GetCpuBrdfLutPixels().empty()
            ? nullptr : &m_skybox->GetCpuBrdfLutPixels();
        frameDesc.brdfLutWidth           = m_skybox->GetCpuBrdfLutWidth();
        frameDesc.brdfLutHeight          = m_skybox->GetCpuBrdfLutHeight();

        float& resolutionScale = settings.hardwareRayTracingResolutionScale;
        if (!settings.rayTracingDynamicResolutionEnabled) {
            resolutionScale = 1.0f;
        } else {
            const float frameMs = (ctx.deltaTime > 0.0f) ? (ctx.deltaTime * 1000.0f) : kTargetRayTracingFrameMs;
            if (frameMs > kTargetRayTracingFrameMs + 0.5f) {
                resolutionScale *= 0.90f;
            } else if (frameMs < kTargetRayTracingFrameMs * 0.75f) {
                resolutionScale *= 1.04f;
            }
            resolutionScale = ClampRayTracingResolutionScale(settings.rayTracingPerformancePreset, resolutionScale);
        }

        frameDesc.dynamicResolutionScale = resolutionScale;
        frameDesc.renderWidth  = ComputeScaledDimension(frameDesc.width,  resolutionScale);
        frameDesc.renderHeight = ComputeScaledDimension(frameDesc.height, resolutionScale);

        const float frameMs = (ctx.deltaTime > 0.0f) ? (ctx.deltaTime * 1000.0f) : kTargetRayTracingFrameMs;
        const RayTracingQualityTier qualityTier =
            ResolveRayTracingQualityTier(settings.rayTracingPerformancePreset, resolutionScale, frameMs);
        frameDesc.qualityTier       = static_cast<uint32_t>(qualityTier);
        frameDesc.pointLightBudget  = ResolvePointLightBudget(qualityTier);
        frameDesc.spotLightBudget   = ResolveSpotLightBudget(qualityTier);
        frameDesc.flags             = kRayTracingFrameFlagDirectionalShadow;
        frameDesc.maxBounceCount    = settings.rayTracingMaxBounceCount;
        frameDesc.samplesPerPixel   = kDefaultRayTracingSamplesPerPixel;

        outRuntimeStats.renderWidth          = frameDesc.renderWidth;
        outRuntimeStats.renderHeight         = frameDesc.renderHeight;
        outRuntimeStats.dynamicResolutionScale = resolutionScale;
        outRuntimeStats.qualityTier          = frameDesc.qualityTier;
    }

    bool SWRTExecutor::ExecuteHardwareImpl(CommandList* cmdList,
                                           UINT backIndex,
                                           const RayTracingFrameDesc& frameDesc,
                                           RayTracingRuntimeStats& outStats)
    {
        if (!cmdList || !m_renderTargetPool->GetRayTracingOutput().IsValid()) {
            return false;
        }

        if (!m_dxrRayTracer->Render(*m_device,
                                    *cmdList,
                                    *m_srvHeap,
                                    m_renderTargetPool->GetRayTracingOutput(),
                                    frameDesc,
                                    outStats)) {
            return false;
        }

        const Resource* backBuffer = m_renderTargetPool->GetBackBufferResource(backIndex);
        if (!backBuffer) {
            return false;
        }

        const auto copyStart = std::chrono::high_resolution_clock::now();
        const auto backBufferToCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer->Get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->ResourceBarrier(1u, &backBufferToCopyDest);
        cmdList->Get()->CopyResource(backBuffer->Get(), m_renderTargetPool->GetRayTracingOutput().Get());
        const auto backBufferToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer->Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PRESENT);
        cmdList->ResourceBarrier(1u, &backBufferToPresent);
        const auto copyEnd = std::chrono::high_resolution_clock::now();

        outStats.usingHardwarePath = true;
        outStats.copyMs    = std::chrono::duration<float, std::milli>(copyEnd - copyStart).count();
        outStats.lastFrameMs = outStats.sceneBuildMs + outStats.traceMs + outStats.copyMs;
        return true;
    }

    bool SWRTExecutor::ExecuteHardware(CommandList* cmdList,
                                       UINT backIndex,
                                       const FrameContext& ctx,
                                       RenderSettings& settings,
                                       RayTracingStats& outStats)
    {
        if (!cmdList || !m_device) {
            return false;
        }

        if (!m_renderTargetPool->EnsureRayTracingOutput(*m_device,
                                                        static_cast<UINT>(ctx.viewportWidth),
                                                        static_cast<UINT>(ctx.viewportHeight))) {
            return false;
        }

        RayTracingRuntimeStats runtimeStats{};
        RayTracingFrameDesc frameDesc{};
        ConfigureFrameDesc(frameDesc, runtimeStats, ctx, settings);

        outStats.hardwareSupported = IsHardwareSupported();
        outStats.instanceCount     = static_cast<uint32_t>(m_rayTracingScene->instances.size());
        outStats.triangleCount     = m_rayTracingScene->TriangleCount();

        if (settings.renderPathMode != RenderPathMode::HardwareRayTracing ||
            !outStats.hardwareSupported ||
            !ExecuteHardwareImpl(cmdList, backIndex, frameDesc, runtimeStats)) {
            return false;
        }

        outStats.usingHardwarePath      = runtimeStats.usingHardwarePath;
        outStats.usedFallback           = runtimeStats.usedFallback;
        outStats.bvhNodeCount           = runtimeStats.bvhNodeCount;
        outStats.renderWidth            = runtimeStats.renderWidth;
        outStats.renderHeight           = runtimeStats.renderHeight;
        outStats.qualityTier            = runtimeStats.qualityTier;
        outStats.dynamicResolutionScale = runtimeStats.dynamicResolutionScale;
        outStats.sceneBuildMs           = runtimeStats.sceneBuildMs;
        outStats.primaryTraceMs         = runtimeStats.primaryTraceMs;
        outStats.shadowTraceMs          = runtimeStats.shadowTraceMs;
        outStats.shadeMs                = runtimeStats.shadeMs;
        outStats.resolveMs              = runtimeStats.resolveMs;
        outStats.traceMs                = runtimeStats.traceMs;
        outStats.copyMs                 = runtimeStats.copyMs;
        outStats.lastFrameMs            = runtimeStats.lastFrameMs;
        return true;
    }

    bool SWRTExecutor::ExecuteDirectionalShadow(CommandList* cmdList,
                                                const LightSystem::ShadowPassContext& shadowContext,
                                                const FrameContext& /*ctx*/,
                                                const PartialBehavior& behavior,
                                                const RenderSettings& settings,
                                                RayTracingStats& outStats)
    {
        if (!cmdList || !m_device) {
            return false;
        }

        const uint32_t shadowMapSize = behavior.shadowMapSize;
        {
            bool cacheInvalidated = false;
            if (!m_renderTargetPool->EnsureSWRTShadow(*m_device, shadowMapSize, cacheInvalidated)) {
                return false;
            }
            if (cacheInvalidated) {
                m_cache.shadowValid = false;
            }
        }

        const uint64_t sceneVersion  = ComputeSceneVersion();
        const uint64_t lightHash     = ComputeShadowLightHash();
        const bool projectionChanged = HasShadowProjectionChanged(shadowContext.lightViewProjection);
        const bool resourceChanged   = !m_cache.shadowValid || m_cache.shadowMapSize != shadowMapSize;
        const bool sceneChanged      = (m_cache.shadowSceneVersion != sceneVersion);
        const bool lightChanged      = (m_cache.shadowLightHash != lightHash);
        const bool shouldUpdate      = resourceChanged || sceneChanged || lightChanged || projectionChanged;

        outStats.shadowMapSize          = shadowMapSize;
        outStats.shadowUpdateInterval   = behavior.shadowUpdateInterval;
        outStats.renderWidth            = shadowMapSize;
        outStats.renderHeight           = shadowMapSize;
        outStats.shadowUpdatedThisFrame = false;
        outStats.shadowReusedThisFrame  = false;

        if (!shouldUpdate) {
            m_cache.framesSinceShadowUpdate = std::min(m_cache.framesSinceShadowUpdate + 1u,
                                                       std::numeric_limits<uint32_t>::max() - 1u);
            outStats.shadowReusedThisFrame = m_cache.shadowValid;
            return true;
        }

        m_gpuSoftwareRayTracer->UpdateScene(*m_rayTracingScene, *m_device, *cmdList);

        const auto shadowToUav = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool->GetSWRTShadowTexture().Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1u, &shadowToUav);

        RayTracingRuntimeStats runtimeStats{};
        bool shadowOk = true;
        const uint32_t cascadeCount = std::max(1u, std::min(shadowContext.cascadeCount,
                                                            LightSystem::kDirectionalCascadeCount));
        for (uint32_t cascadeIndex = 0u; cascadeIndex < cascadeCount; ++cascadeIndex)
        {
            GpuSoftwareRayTracer::DirectionalShadowMapDesc shadowDesc{};
            shadowDesc.width  = shadowMapSize;
            shadowDesc.height = shadowMapSize;
            shadowDesc.arraySlice = cascadeIndex;
            shadowDesc.constantBufferSlot = 2u + cascadeIndex;
            shadowDesc.depthBias = 0.0f;
            std::memcpy(shadowDesc.lightViewProjection,
                        shadowContext.cascadeLightViewProjection[cascadeIndex],
                        sizeof(shadowDesc.lightViewProjection));
            if (!Math::Invert4x4(shadowDesc.lightViewProjection, shadowDesc.inverseLightViewProjection))
            {
                shadowOk = false;
                break;
            }

            RayTracingRuntimeStats cascadeStats{};
            if (!m_gpuSoftwareRayTracer->RenderDirectionalShadowMap(shadowDesc,
                                                                     *m_device,
                                                                     *cmdList,
                                                                     m_renderTargetPool->GetSWRTShadowTexture(),
                                                                     cascadeStats))
            {
                shadowOk = false;
                break;
            }

            const auto cascadeUavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(
                m_renderTargetPool->GetSWRTShadowTexture().Get());
            cmdList->ResourceBarrier(1u, &cascadeUavBarrier);

            runtimeStats.bvhNodeCount = std::max(runtimeStats.bvhNodeCount, cascadeStats.bvhNodeCount);
            runtimeStats.traceMs += cascadeStats.traceMs;
            runtimeStats.copyMs += cascadeStats.copyMs;
            runtimeStats.lastFrameMs += cascadeStats.lastFrameMs;
        }

        const auto shadowToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool->GetSWRTShadowTexture().Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1u, &shadowToSrv);

        if (!shadowOk) {
            return false;
        }

        outStats.hardwareSupported      = IsHardwareSupported();
        outStats.usingHardwarePath      = false;
        outStats.usedFallback           = false;
        outStats.instanceCount          = static_cast<uint32_t>(m_rayTracingScene->instances.size());
        outStats.triangleCount          = m_rayTracingScene->TriangleCount();
        outStats.bvhNodeCount           = std::max(outStats.bvhNodeCount, runtimeStats.bvhNodeCount);
        outStats.renderWidth            = shadowMapSize;
        outStats.renderHeight           = shadowMapSize;
        outStats.qualityTier            = static_cast<uint32_t>(settings.rayTracingPerformancePreset);
        outStats.dynamicResolutionScale = 1.0f;
        outStats.sceneBuildMs          += runtimeStats.sceneBuildMs;
        outStats.shadowTraceMs         += runtimeStats.traceMs;
        outStats.traceMs               += runtimeStats.traceMs;
        outStats.lastFrameMs            = outStats.sceneBuildMs + outStats.traceMs + outStats.copyMs;
        outStats.shadowUpdatedThisFrame = true;

        m_cache.shadowValid               = true;
        m_cache.framesSinceShadowUpdate   = 0u;
        m_cache.shadowMapSize             = shadowMapSize;
        m_cache.shadowSceneVersion        = sceneVersion;
        m_cache.shadowLightHash           = lightHash;
        std::memcpy(m_cache.shadowLightViewProjection,
                    shadowContext.lightViewProjection,
                    sizeof(m_cache.shadowLightViewProjection));
        return true;
    }

    bool SWRTExecutor::ExecuteReflections(CommandList* cmdList,
                                          const FrameContext& ctx,
                                          const PartialBehavior& behavior,
                                          const RenderSettings& settings,
                                          RayTracingStats& outStats)
    {
        if (!cmdList || !m_device) {
            return false;
        }

        const bool reflectionDebugView = UsesReflectionDebugView(settings.gBufferDebugView);
        const bool finalLitReflectionView =
            settings.gBufferDebugView == RendererEnums::GBufferDebugView::FinalLit;
        const bool forceDenoisedFinalReflection =
            finalLitReflectionView && settings.swrtDenoiserEnabled;
        const float reflectionScale = reflectionDebugView ? 1.0f : behavior.reflectionResolutionScale;
        const uint32_t reflectionWidth  = ComputeScaledDimension(static_cast<uint32_t>(ctx.viewportWidth),  reflectionScale);
        const uint32_t reflectionHeight = ComputeScaledDimension(static_cast<uint32_t>(ctx.viewportHeight), reflectionScale);
        {
            bool cacheInvalidated = false;
            if (!m_renderTargetPool->EnsureSWRTReflection(*m_device, reflectionWidth, reflectionHeight, cacheInvalidated)) {
                return false;
            }
            if (cacheInvalidated) {
                m_cache.reflectionValid = false;
            }
        }

        const uint64_t sceneVersion   = ComputeSceneVersion();
        const uint64_t lightingHash   = ComputeReflectionLightingHash(settings);
        const bool resourceChanged    = !m_cache.reflectionValid ||
                                        m_cache.reflectionWidth  != reflectionWidth ||
                                        m_cache.reflectionHeight != reflectionHeight;
        const bool sceneChanged       = (m_cache.reflectionSceneVersion  != sceneVersion);
        const bool lightingChanged    = (m_cache.reflectionLightingHash   != lightingHash);
        const bool cameraChanged      = m_cache.reflectionValid && HasReflectionCameraChanged(ctx.cameraPos, ctx.cameraInvPV);
        const bool forceFullRefresh   = resourceChanged || sceneChanged || lightingChanged || cameraChanged ||
                                        reflectionDebugView || forceDenoisedFinalReflection;
        const bool needsRefresh       = forceFullRefresh || m_cache.reflectionPendingPhasePasses > 0u;
        const bool intervalSatisfied  = reflectionDebugView || forceDenoisedFinalReflection ||
                                        (m_cache.framesSinceReflectionUpdate + 1u) >= behavior.reflectionUpdateInterval;

        outStats.reflectionWidth           = reflectionWidth;
        outStats.reflectionHeight          = reflectionHeight;
        outStats.reflectionResolutionScale = reflectionScale;
        outStats.reflectionUpdateInterval  = behavior.reflectionUpdateInterval;
        outStats.reflectionPhaseCount      = behavior.reflectionPhaseCount;
        outStats.reflectionMaxRoughness    = behavior.reflectionMaxRoughness;
        outStats.reflectionMinEnergy       = behavior.reflectionMinEnergy;
        outStats.reflectionMaxDistance     = behavior.reflectionMaxTraceDistance;
        outStats.renderWidth               = reflectionWidth;
        outStats.renderHeight              = reflectionHeight;
        outStats.dynamicResolutionScale    = reflectionScale;
        outStats.reflectionUpdatedThisFrame = false;
        outStats.reflectionReusedThisFrame  = false;

        if (!needsRefresh || (!forceFullRefresh && !intervalSatisfied)) {
            m_cache.framesSinceReflectionUpdate = std::min(m_cache.framesSinceReflectionUpdate + 1u,
                                                           std::numeric_limits<uint32_t>::max() - 1u);
            outStats.reflectionReusedThisFrame = m_cache.reflectionValid;
            outStats.reflectionPhaseIndex      = m_cache.reflectionPhaseIndex;
            return true;
        }

        // No WaitForGPU needed: descriptor slots 14-133 are double-buffered by frame index
        // (even frames use slots 14-73, odd frames use slots 74-133).
        // CPU writes frame N's slot while GPU reads frame N-1's slot  Eno race condition.

        GpuSoftwareRayTracer::ReflectionTextureDesc reflectionDesc{};
        reflectionDesc.width              = reflectionWidth;
        reflectionDesc.height             = reflectionHeight;
        reflectionDesc.updatePhaseCount   = forceFullRefresh ? 1u : std::max(1u, behavior.reflectionPhaseCount);
        reflectionDesc.updatePhaseIndex   = forceFullRefresh
            ? 0u
            : (m_cache.reflectionPhaseIndex % std::max(1u, behavior.reflectionPhaseCount));
        reflectionDesc.maxSurfaceRoughness       = behavior.reflectionMaxRoughness;
        reflectionDesc.minReflectionEnergy       = behavior.reflectionMinEnergy;
        reflectionDesc.maxPrimaryHitDistance     = behavior.reflectionMaxHitDistance;
        reflectionDesc.maxReflectionTraceDistance = behavior.reflectionMaxTraceDistance;
        reflectionDesc.samplesPerPixel            = settings.swrtSamplesPerPixel;
        reflectionDesc.samplingMode               = settings.swrtSamplingMode;
        reflectionDesc.maxBounces                 = settings.swrtMaxBounces;
        reflectionDesc.debugView                  = static_cast<uint32_t>(settings.gBufferDebugView);
        reflectionDesc.cameraChanged              = cameraChanged;
        reflectionDesc.preserveExistingPixels     = !forceFullRefresh;
        reflectionDesc.denoiserEnabled            = reflectionDebugView ? false : settings.swrtDenoiserEnabled;
        reflectionDesc.temporalAlpha              = settings.swrtReflectionTemporalAlpha;
        reflectionDesc.atrousIterations           = reflectionDebugView ? 0u : settings.swrtReflectionAtrousIterations;
        reflectionDesc.atrousPhiDepth             = settings.swrtReflectionAtrousPhiDepth;
        reflectionDesc.iblEnabled                 = m_skybox && m_skybox->IsIblEnabled();
        reflectionDesc.proceduralSkyEnabled       = m_skybox && !m_skybox->IsIblEnabled();
        reflectionDesc.iblIntensity               = settings.iblIntensity;
        reflectionDesc.iblPrefilterMaxMip         = m_skybox ? m_skybox->GetIblPrefilterMaxMip() : 0.0f;
        reflectionDesc.frameDesc.directionalLight = m_lightSystem->GetDirectionalLightSettings();
        reflectionDesc.frameDesc.pointLights      = &m_lightSystem->GetPointLights();
        reflectionDesc.frameDesc.spotLights       = &m_lightSystem->GetSpotLights();
        std::memcpy(reflectionDesc.frameDesc.inverseViewProjection, ctx.cameraInvPV, sizeof(ctx.cameraInvPV));
        std::memcpy(reflectionDesc.frameDesc.cameraPosition, ctx.cameraPos, sizeof(ctx.cameraPos));

        reflectionDesc.gbufferNormalTex   = m_renderTargetPool->GetGBufferNormal().IsValid()   ? m_renderTargetPool->GetGBufferNormal().Get()   : nullptr;
        reflectionDesc.gbufferMaterialTex = m_renderTargetPool->GetGBufferMaterial().IsValid() ? m_renderTargetPool->GetGBufferMaterial().Get() : nullptr;
        reflectionDesc.gbufferAlbedoTex   = m_renderTargetPool->GetGBufferAlbedo().IsValid()   ? m_renderTargetPool->GetGBufferAlbedo().Get()   : nullptr;
        reflectionDesc.iblPrefilterTex    = (m_skybox && m_skybox->IsIblEnabled()) ? m_skybox->GetIblPrefilterResource() : nullptr;
        reflectionDesc.gbufferWidth       = static_cast<uint32_t>(ctx.viewportWidth);
        reflectionDesc.gbufferHeight      = static_cast<uint32_t>(ctx.viewportHeight);

        m_gpuSoftwareRayTracer->UpdateScene(*m_rayTracingScene, *m_device, *cmdList);

        // GI probe update (reuses the freshly-built BVH)
        {
            const auto bvhAddrs = m_gpuSoftwareRayTracer->GetBvhGpuAddresses();
            if (bvhAddrs.valid && m_probeGrid->IsInitialized())
            {
                const auto dirLight = m_lightSystem->GetDirectionalLightSettings();
                float fwd[3];
                Math::DirectionFromYawPitch(dirLight.yaw, dirLight.pitch, fwd);
                IrradianceProbeGrid::UpdateDesc giDesc{};
                giDesc.dirLightDir[0]    = -fwd[0];
                giDesc.dirLightDir[1]    = -fwd[1];
                giDesc.dirLightDir[2]    = -fwd[2];
                giDesc.dirLightIntensity = dirLight.intensity;
                giDesc.dirLightColor[0]  = dirLight.color[0];
                giDesc.dirLightColor[1]  = dirLight.color[1];
                giDesc.dirLightColor[2]  = dirLight.color[2];
                giDesc.ambientColor[0]   = 0.1f;
                giDesc.ambientColor[1]   = 0.1f;
                giDesc.ambientColor[2]   = 0.1f;
                giDesc.ambientIntensity  = 1.0f;
                giDesc.shadowBias        = 0.005f;
                giDesc.frameIndex        = m_giFrameIndex++;
                m_probeGrid->UpdateProbes(giDesc, bvhAddrs, *m_device, *cmdList);
            }
        }

        // Reflections run immediately after the lighting draw while the G-Buffer is
        // still in render-target state, so transition from RENDER_TARGET here.
        if (m_renderTargetPool->GetGBufferNormal().IsValid() &&
            m_renderTargetPool->GetGBufferMaterial().IsValid() &&
            m_renderTargetPool->GetGBufferAlbedo().IsValid())
        {
            D3D12_RESOURCE_BARRIER toNps[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferNormal().Get(),
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferMaterial().Get(),
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferAlbedo().Get(),
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cmdList->ResourceBarrier(_countof(toNps), toNps);
        }

        const auto reflectionToUav = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool->GetSWRTReflectionTexture().Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1u, &reflectionToUav);

        RayTracingRuntimeStats runtimeStats{};
        const bool reflOk = m_gpuSoftwareRayTracer->RenderReflectionTexture(reflectionDesc,
                                                                             *m_device,
                                                                             *cmdList,
                                                                             m_renderTargetPool->GetSWRTReflectionTexture(),
                                                                             runtimeStats);

        // Always restore resource states regardless of whether the reflection pass succeeded.
        // Leaving the reflection texture in UAV state or the G-Buffer textures in
        // NON_PIXEL_SHADER_RESOURCE state would cause validation errors on subsequent frames.
        const auto reflectionToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool->GetSWRTReflectionTexture().Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1u, &reflectionToSrv);

        if (reflOk && settings.swrtUseReSTIR)
        {
            if (m_renderTargetPool->EnsureReSTIRShadow(*m_device, reflectionWidth, reflectionHeight))
            {
                const auto shadowToUav = CD3DX12_RESOURCE_BARRIER::Transition(
                    m_renderTargetPool->GetReSTIRShadowTexture().Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmdList->ResourceBarrier(1u, &shadowToUav);

                // RenderReflectionTexture() already populated the internal ReSTIR
                // GBuffer when ReSTIR mode is active. Reusing it here avoids a
                // second full ReSTIR reflection dispatch on the shadow texture.
                RayTracingRuntimeStats shadowStats{};
                if (!m_gpuSoftwareRayTracer->RenderShadowReSTIR(reflectionDesc,
                                                                 *m_device,
                                                                 *cmdList,
                                                                 m_renderTargetPool->GetReSTIRShadowTexture(),
                                                                 shadowStats))
                {
                    DebugLog("SWRTExecutor::ExecuteReflections: ReSTIR shadow pass failed.\n");
                }

                const auto shadowToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    m_renderTargetPool->GetReSTIRShadowTexture().Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cmdList->ResourceBarrier(1u, &shadowToSrv);
            }
        }

        // Restore the render graph's expected state. Final graph cleanup will later
        // transition these resources to PIXEL_SHADER_RESOURCE for the next frame.
        if (m_renderTargetPool->GetGBufferNormal().IsValid() &&
            m_renderTargetPool->GetGBufferMaterial().IsValid() &&
            m_renderTargetPool->GetGBufferAlbedo().IsValid())
        {
            D3D12_RESOURCE_BARRIER toRenderTarget[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferNormal().Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferMaterial().Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferAlbedo().Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET),
            };
            cmdList->ResourceBarrier(_countof(toRenderTarget), toRenderTarget);
        }

        if (!reflOk) {
            DebugLog("SWRTExecutor::ExecuteReflections: RenderReflectionTexture failed.\n");
            return false;
        }

        outStats.hardwareSupported       = IsHardwareSupported();
        outStats.usingHardwarePath       = false;
        outStats.usedFallback            = false;
        outStats.instanceCount           = static_cast<uint32_t>(m_rayTracingScene->instances.size());
        outStats.triangleCount           = m_rayTracingScene->TriangleCount();
        outStats.bvhNodeCount            = std::max(outStats.bvhNodeCount, runtimeStats.bvhNodeCount);
        outStats.renderWidth             = reflectionWidth;
        outStats.renderHeight            = reflectionHeight;
        outStats.qualityTier             = runtimeStats.qualityTier;
        outStats.dynamicResolutionScale  = reflectionScale;
        outStats.sceneBuildMs           += runtimeStats.sceneBuildMs;
        outStats.traceMs                += runtimeStats.traceMs;
        outStats.lastFrameMs             = outStats.sceneBuildMs + outStats.traceMs + outStats.copyMs;
        outStats.reflectionUpdatedThisFrame = true;
        outStats.reflectionPhaseIndex    = reflectionDesc.updatePhaseIndex;

        m_cache.reflectionValid                = true;
        m_cache.framesSinceReflectionUpdate    = 0u;
        m_cache.reflectionWidth                = reflectionWidth;
        m_cache.reflectionHeight               = reflectionHeight;
        m_cache.reflectionSceneVersion         = sceneVersion;
        m_cache.reflectionLightingHash         = lightingHash;
        std::memcpy(m_cache.reflectionCameraPosition, ctx.cameraPos, sizeof(ctx.cameraPos));
        std::memcpy(m_cache.reflectionInverseViewProjection, ctx.cameraInvPV, sizeof(ctx.cameraInvPV));
        if (forceFullRefresh) {
            m_cache.reflectionPendingPhasePasses = 0u;
            m_cache.reflectionPhaseIndex         = 0u;
        } else {
            if (m_cache.reflectionPendingPhasePasses > 0u) {
                --m_cache.reflectionPendingPhasePasses;
            }
            m_cache.reflectionPhaseIndex =
                (m_cache.reflectionPhaseIndex + 1u) % std::max(1u, behavior.reflectionPhaseCount);
        }
        return true;
    }

    bool SWRTExecutor::ExecuteAmbientOcclusion(CommandList* cmdList,
                                               const FrameContext& ctx,
                                               const RenderSettings& settings,
                                               RayTracingStats& outStats)
    {
        if (!cmdList || !m_device) {
            return false;
        }

        const uint32_t width = static_cast<uint32_t>(ctx.viewportWidth);
        const uint32_t height = static_cast<uint32_t>(ctx.viewportHeight);
        bool cacheInvalidated = false;
        if (!m_renderTargetPool->EnsureSWRTAmbientOcclusion(*m_device, width, height, cacheInvalidated)) {
            return false;
        }

        GpuSoftwareRayTracer::AmbientOcclusionTextureDesc aoDesc{};
        aoDesc.width = width;
        aoDesc.height = height;
        std::memcpy(aoDesc.inverseViewProjection, ctx.cameraInvPV, sizeof(ctx.cameraInvPV));
        std::memcpy(aoDesc.cameraPosition, ctx.cameraPos, sizeof(ctx.cameraPos));
        aoDesc.gbufferNormalTex   = m_renderTargetPool->GetGBufferNormal().IsValid()   ? m_renderTargetPool->GetGBufferNormal().Get()   : nullptr;
        aoDesc.gbufferMaterialTex = m_renderTargetPool->GetGBufferMaterial().IsValid() ? m_renderTargetPool->GetGBufferMaterial().Get() : nullptr;
        aoDesc.gbufferAlbedoTex   = m_renderTargetPool->GetGBufferAlbedo().IsValid()   ? m_renderTargetPool->GetGBufferAlbedo().Get()   : nullptr;
        aoDesc.gbufferWidth  = width;
        aoDesc.gbufferHeight = height;
        aoDesc.tMin = std::max(1e-4f, settings.runtimeAoBias);
        aoDesc.radius = std::max(0.05f, settings.runtimeAoRadius);
        aoDesc.power = std::max(0.25f, settings.runtimeAoIntensity);
        uint32_t swrtAoSampleCount = settings.swrtAoSampleCount;
        switch (settings.rayTracingPerformancePreset) {
        case RayTracingPerformancePreset::UltraFast:
            swrtAoSampleCount = std::min(swrtAoSampleCount, 8u);
            break;
        case RayTracingPerformancePreset::Performance:
            swrtAoSampleCount = std::min(swrtAoSampleCount, 16u);
            break;
        case RayTracingPerformancePreset::Balanced:
        default:
            break;
        }
        aoDesc.sampleCount = std::max(4u, swrtAoSampleCount);
        aoDesc.frameIndex = m_giFrameIndex;

        m_gpuSoftwareRayTracer->UpdateScene(*m_rayTracingScene, *m_device, *cmdList);

        if (m_renderTargetPool->GetGBufferNormal().IsValid() &&
            m_renderTargetPool->GetGBufferMaterial().IsValid() &&
            m_renderTargetPool->GetGBufferAlbedo().IsValid()) {
            D3D12_RESOURCE_BARRIER toNps[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferNormal().Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferMaterial().Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferAlbedo().Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cmdList->ResourceBarrier(_countof(toNps), toNps);
        }

        const auto aoToUav = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool->GetSWRTAmbientOcclusionTexture().Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1u, &aoToUav);

        RayTracingRuntimeStats runtimeStats{};
        const bool ok = m_gpuSoftwareRayTracer->RenderAmbientOcclusionTexture(aoDesc,
                                                                              *m_device,
                                                                              *cmdList,
                                                                              m_renderTargetPool->GetSWRTAmbientOcclusionTexture(),
                                                                              runtimeStats);

        const auto aoToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool->GetSWRTAmbientOcclusionTexture().Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1u, &aoToSrv);

        if (m_renderTargetPool->GetGBufferNormal().IsValid() &&
            m_renderTargetPool->GetGBufferMaterial().IsValid() &&
            m_renderTargetPool->GetGBufferAlbedo().IsValid()) {
            D3D12_RESOURCE_BARRIER toPsr[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferNormal().Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferMaterial().Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool->GetGBufferAlbedo().Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            cmdList->ResourceBarrier(_countof(toPsr), toPsr);
        }

        if (!ok) {
            return false;
        }

        outStats.hardwareSupported = IsHardwareSupported();
        outStats.usingHardwarePath = false;
        outStats.instanceCount = static_cast<uint32_t>(m_rayTracingScene->instances.size());
        outStats.triangleCount = m_rayTracingScene->TriangleCount();
        outStats.bvhNodeCount = std::max(outStats.bvhNodeCount, runtimeStats.bvhNodeCount);
        outStats.renderWidth = width;
        outStats.renderHeight = height;
        outStats.traceMs += runtimeStats.traceMs;
        outStats.sceneBuildMs += runtimeStats.sceneBuildMs;
        outStats.lastFrameMs = outStats.sceneBuildMs + outStats.traceMs + outStats.copyMs;
        return true;
    }
}
