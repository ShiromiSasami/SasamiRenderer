// RendererDrawCallbacks.cpp
// Renderer::PrepareFrameDrawCallbacks -- stage 4 of Renderer::Render's frame graph
// construction: builds the static-mesh draw/shadow-draw callbacks used by the
// render-pass graph and resolves this frame's software ray-traced shadow/reflection/AO
// feature gates. See Renderer.h for why this lives in its own file, and
// FrameRenderContext.h for why stage 4 and stage 5 (RendererPassServices.cpp) hand
// state to each other through a shared context struct instead of ordinary locals.
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Runtime/FrameRenderContext.h"
#include "Renderer/Culling/Frustum.h"
#include "Renderer/Scene/MaterialConstants.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"

// windows.h (pulled in transitively by the D3D12 headers) defines min/max as macros, which
// breaks std::max and std::numeric_limits<T>::max(). Same guard as RendererFrameGraph.cpp.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif


namespace SasamiEngine::Render
{
    namespace
    {
        // Periodic culling report. Sampled rather than per-frame because these loops run several
        // times a frame (one per cascade, plus opaque and transparent) and a line each would bury
        // the rest of the log. The point is to make the effect of culling measurable: before this
        // existed the shadow pass drew meshCount * cascadeCount every frame with no way to see it.
        // The sample counter is per-label. A single shared counter cannot be used here: the
        // call sites have different call rates (four cascades per frame vs one opaque pass),
        // so one counter makes each label's samples land on an arbitrary subset of frames and
        // cascades. Reading those lines as a per-cascade breakdown of one frame silently
        // produces a wrong draw-call total -- they are unrelated samples from unrelated frames.
        void LogCullingStats(const char* label, size_t considered, size_t drawn)
        {
            static std::unordered_map<std::string, unsigned int> s_sampleCounters;
            constexpr unsigned int kSampleInterval = 240u;
            if ((s_sampleCounters[label]++ % kSampleInterval) != 0u) {
                return;
            }
            char line[160];
            std::snprintf(line, sizeof(line), "[Culling] %s: drawn=%zu / considered=%zu (culled %zu)\n",
                          label, drawn, considered, considered - drawn);
            DebugLog(line);
        }

        uint32_t ComputeScaledDimension(uint32_t dimension, float scale)
        {
            return std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(dimension) * scale)));
        }

        bool UsesRayTracedRuntimeAmbientOcclusion(RendererEnums::AmbientOcclusionMode mode,
                                                  RendererEnums::RuntimeAmbientOcclusionMethod method)
        {
            return mode != RendererEnums::AmbientOcclusionMode::MaterialOnly &&
                   (method == RendererEnums::RuntimeAmbientOcclusionMethod::RayTraced ||
                    mode == RendererEnums::AmbientOcclusionMode::RayTracedAOOnly);
        }

        bool UsesReflectionDebugView(RendererEnums::GBufferDebugView view)
        {
            return view == RendererEnums::GBufferDebugView::ReflectionRadiance ||
                   view == RendererEnums::GBufferDebugView::ReflectionAlpha ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionHitDistance ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionComposite;
        }
    }

    using Math::Mul4x4;

    void Renderer::BindShadowAlphaMaskAlbedo(D3D12CommandListRhiEncoder& enc, Texture* texture)
    {
        if (texture) {
            enc.SetGraphicsDescriptorTable(0, { texture->srv.ptr });
        } else if (m_defaultAlbedoTexture) {
            enc.SetGraphicsDescriptorTable(0, { m_defaultAlbedoTexture->srv.ptr });
        } else {
            enc.SetGraphicsDescriptorTable(0, { m_nullTextureSrv.ptr });
        }
    }

    void Renderer::PrepareFrameDrawCallbacks(FrameRenderContext& context)
    {
        CommandList* const cmdList = *context.cmdList;
        auto* const frame = context.frame;

        context.graphicsCommandEncoder.emplace(*m_device, *cmdList);
        D3D12CommandListRhiEncoder& graphicsCommandEncoder = *context.graphicsCommandEncoder;

        // Runtime AO SRV selection is shared by the static and skinned draw lambdas: both bind
        // it at slot 8 (t9) once per batch, and the skinned transparent PS samples it too.
        context.selectRuntimeAoSrv = [this]() -> GpuDescriptorHandle {
            GpuDescriptorHandle runtimeAoSrv = m_nullTextureSrv;
            // Switch on the effective mode, not the raw setting: with runtimeAoEnabled off the raw
            // mode still reached the RuntimeAOOnly/Hybrid and RayTracedAOOnly arms and bound the last
            // SSAO/SWRT texture, which no longer refreshes -- a frozen frame's worth of occlusion.
            const RendererEnums::AmbientOcclusionMode effectiveAoMode = GetEffectiveAmbientOcclusionMode();
            const bool usesRayTracedAo =
                UsesRayTracedRuntimeAmbientOcclusion(effectiveAoMode, m_settings.runtimeAoMethod);
            switch (effectiveAoMode) {
            case RendererEnums::AmbientOcclusionMode::MaterialOnly:
                runtimeAoSrv = m_nullTextureSrv;
                break;
            case RendererEnums::AmbientOcclusionMode::RuntimeAOOnly:
            case RendererEnums::AmbientOcclusionMode::Hybrid:
            {
                // Fallback to white (AO=1) rather than null (AO=0) when runtime AO is not ready.
                const GpuDescriptorHandle runtimeAoFallback = m_defaultOcclusionTexture
                    ? m_defaultOcclusionTexture->srv : m_nullTextureSrv;
                if (usesRayTracedAo) {
                    runtimeAoSrv =
                        m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                            ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                            : runtimeAoFallback;
                } else if (m_settings.gBufferDebugView == RendererEnums::GBufferDebugView::RuntimeAmbientOcclusionRaw) {
                    runtimeAoSrv = m_renderTargetPool.GetSSAOTexture().IsValid()
                        ? m_renderTargetPool.GetSSAOSrv()
                        : runtimeAoFallback;
                } else {
                    runtimeAoSrv =
                        m_renderTargetPool.GetSSAOBlurTexture().IsValid() ? m_renderTargetPool.GetSSAOBlurSrv()
                        : m_renderTargetPool.GetSSAOTexture().IsValid() ? m_renderTargetPool.GetSSAOSrv()
                        : runtimeAoFallback;
                }
                break;
            }
            case RendererEnums::AmbientOcclusionMode::RayTracedAOOnly:
                runtimeAoSrv =
                    m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                        ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                        : m_nullTextureSrv;
                break;
            default:
                break;
            }
            return runtimeAoSrv;
        };
        auto& selectRuntimeAoSrv = context.selectRuntimeAoSrv;

        context.drawItems = [this, &graphicsCommandEncoder, frame, &selectRuntimeAoSrv](bool drawTransparent,
                                                                                         bool dynamicVerticesOnly) {
            auto* enc = &graphicsCommandEncoder;
            // Bind runtime-generated AO texture at slot 8 (t9 in shader) once per batch.
            enc->SetGraphicsDescriptorTable(8, { selectRuntimeAoSrv().ptr });

            // Bind GI probe grid CB (b2) and probe SH data (t10) as inline root descriptors.
            // These are always bound so PBR_PS can read g_giEnabled to decide whether to use probes.
            if (m_readyState.IsFeatureReady(m_readyState.giReady) && m_probeGrid.IsInitialized()) {
                const RhiGpuAddress probeCbGpu = m_probeGrid.GetProbeGridCbGpuAddress();
                const RhiGpuAddress probeVA    = m_probeGrid.GetProbeDataGpuVA();
                if (probeCbGpu != 0) enc->SetGraphicsConstantBufferView(9, probeCbGpu);
                if (probeVA    != 0) enc->SetGraphicsShaderResourceView(10, probeVA);
            }

            // Frustum-cull against the camera before building the draw list. Bistro submits
            // ~1700 opaque meshes and every one of them used to be drawn every frame no matter
            // where the camera looked, which is most of the 8500 draw calls a frame was costing.
            //
            // Items whose bounds could not be computed (hasWorldBounds == false, e.g. a mesh with
            // no vertices) are always kept: a false positive only costs a draw, a false negative
            // makes geometry vanish.
            const Culling::FrustumPlanes cameraFrustum =
                Culling::ExtractFrustumPlanes(m_cameraState.GetPV());

            std::vector<const SceneSubmitter::DrawItem*> drawList;
            drawList.reserve(m_sceneSubmitter.GetDrawItems().size());
            size_t consideredCount = 0;
            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.transparent != drawTransparent) {
                    continue;
                }
                if (dynamicVerticesOnly && !item.dynamicVertices) {
                    continue;
                }
                ++consideredCount;
                if (item.hasWorldBounds &&
                    !Culling::IsAabbVisible(cameraFrustum, item.worldBoundsMin, item.worldBoundsMax)) {
                    continue;
                }
                drawList.push_back(&item);
            }
            LogCullingStats(drawTransparent ? "Transparent" : "Opaque", consideredCount, drawList.size());
            if (drawTransparent) {
                const float* cameraPos = m_cameraState.GetPos();
                std::sort(drawList.begin(), drawList.end(),
                    [cameraPos](const SceneSubmitter::DrawItem* lhs, const SceneSubmitter::DrawItem* rhs) {
                        const auto distanceSq = [cameraPos](const SceneSubmitter::DrawItem* item) {
                            const float dx = item->model[12] - cameraPos[0];
                            const float dy = item->model[13] - cameraPos[1];
                            const float dz = item->model[14] - cameraPos[2];
                            return dx * dx + dy * dy + dz * dz;
                        };
                        return distanceSq(lhs) > distanceSq(rhs);
                    });
            }

            for (const SceneSubmitter::DrawItem* drawItem : drawList) {
                const auto& item = *drawItem;

                const RendererEnums::AmbientOcclusionMode effectiveAoMode =
                    GetEffectiveAmbientOcclusionMode();
                const MaterialConstants mc = PackMaterialConstants(item, effectiveAoMode);
                float objMVP[16];
                Mul4x4(item.model, m_cameraState.GetPV(), objMVP);
                const RhiGpuAddress cameraCbGpu =
                    m_frameCoordinator.PushCameraCB(*frame, objMVP, item.model,
                        mc.extra[0], mc.extra[1], mc.extra[2], mc.extra[3], mc.extra[4], mc.extra[5], mc.extra[6]);
                if (cameraCbGpu != 0) {
                    enc->SetGraphicsConstantBufferView(2, cameraCbGpu);
                }

                if (item.texture) {
                    enc->SetGraphicsDescriptorTable(0, { item.texture->srv.ptr });
                } else if (m_defaultAlbedoTexture) {
                    enc->SetGraphicsDescriptorTable(0, { m_defaultAlbedoTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(0, { m_nullTextureSrv.ptr });
                }

                if (item.occlusionTexture) {
                    enc->SetGraphicsDescriptorTable(6, { item.occlusionTexture->srv.ptr });
                } else if (m_defaultOcclusionTexture) {
                    enc->SetGraphicsDescriptorTable(6, { m_defaultOcclusionTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(6, { m_nullTextureSrv.ptr });
                }

                if (item.normalTexture) {
                    enc->SetGraphicsDescriptorTable(16, { item.normalTexture->srv.ptr });
                } else if (m_defaultNormalTexture) {
                    enc->SetGraphicsDescriptorTable(16, { m_defaultNormalTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(16, { m_nullTextureSrv.ptr });
                }

                if (item.emissiveTexture) {
                    enc->SetGraphicsDescriptorTable(17, { item.emissiveTexture->srv.ptr });
                } else if (m_defaultEmissiveTexture) {
                    enc->SetGraphicsDescriptorTable(17, { m_defaultEmissiveTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(17, { m_nullTextureSrv.ptr });
                }

                // t19 SDF face-shadow map: scene-wide, white when unset (the shader gates on flags).
                if (m_toonFaceShadowMap) {
                    enc->SetGraphicsDescriptorTable(18, { m_toonFaceShadowMap->srv.ptr });
                } else if (m_defaultEmissiveTexture) {
                    enc->SetGraphicsDescriptorTable(18, { m_defaultEmissiveTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(18, { m_nullTextureSrv.ptr });
                }

                m_meshBuffer.Bind(enc, item.meshIndex);
                const auto& items = m_meshBuffer.Items();
                if (item.meshIndex < items.size()) {
                    const auto& it = items[item.meshIndex];
                    if (it.indexCount > 0) {
                        enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
                    } else if (it.vertexCount > 0) {
                        enc->Draw({ it.vertexCount, 1, 0, 0 });
                    }
                }
            }
        };

        const Resource* lightCB = frame->light.GetLightCBResource();
        context.lightCbGpu = (lightCB && lightCB->IsValid())
            ? lightCB->GetGPUVirtualAddress()
            : 0;
        context.defaultAoSrv =
            (m_defaultOcclusionTexture != nullptr) ? m_defaultOcclusionTexture->srv : m_nullTextureSrv;

        context.partialBehavior =
            m_swrtExecutor.ResolveBehavior(m_settings.rayTracingPerformancePreset);
        const SWRTExecutor::PartialBehavior& partialBehavior = context.partialBehavior;
        const bool swrtReadyForFrame = m_readyState.IsFeatureReady(m_readyState.swrtReady);
        const bool useSoftwareRayTracedDirectionalShadow =
            swrtReadyForFrame &&
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled;
        context.useSoftwareRayTracedReflections =
            swrtReadyForFrame &&
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled;
        context.useSoftwareRayTracedAmbientOcclusion =
            swrtReadyForFrame &&
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.runtimeAoEnabled &&
            UsesRayTracedRuntimeAmbientOcclusion(m_settings.ambientOcclusionMode,
                                                 m_settings.runtimeAoMethod);
        const bool useSoftwareRayTracedReflections = context.useSoftwareRayTracedReflections;
        const bool useSoftwareRayTracedAmbientOcclusion = context.useSoftwareRayTracedAmbientOcclusion;
        if (useSoftwareRayTracedDirectionalShadow) {
            bool cacheInvalidated = false;
            if (!m_renderTargetPool.EnsureSWRTShadow(*m_device, partialBehavior.shadowMapSize, cacheInvalidated)) {
                DebugLog("Renderer::Render: failed to prepare software directional shadow resources. Falling back to raster shadow map.\n");
            }
            if (cacheInvalidated) {
                m_swrtExecutor.OnShadowResourcesReallocated();
            }
        }
        if (useSoftwareRayTracedReflections) {
            const float reflectionScale = UsesReflectionDebugView(m_settings.gBufferDebugView)
                ? 1.0f
                : partialBehavior.reflectionResolutionScale;
            const uint32_t reflectionWidth  = ComputeScaledDimension(
                static_cast<uint32_t>(m_viewport.Width),  reflectionScale);
            const uint32_t reflectionHeight = ComputeScaledDimension(
                static_cast<uint32_t>(m_viewport.Height), reflectionScale);
            bool reflCacheInvalidated = false;
            if (!m_renderTargetPool.EnsureSWRTReflection(*m_device, reflectionWidth, reflectionHeight, reflCacheInvalidated)) {
                DebugLog("Renderer::Render: failed to prepare software reflection resources. Disabling SWRT reflections for this frame.\n");
            }
            if (reflCacheInvalidated) {
                m_swrtExecutor.OnReflectionResourcesReallocated();
            }
        }
        if (useSoftwareRayTracedAmbientOcclusion) {
            bool aoCacheInvalidated = false;
            if (!m_renderTargetPool.EnsureSWRTAmbientOcclusion(*m_device,
                                                               static_cast<uint32_t>(m_viewport.Width),
                                                               static_cast<uint32_t>(m_viewport.Height),
                                                               aoCacheInvalidated)) {
                DebugLog("Renderer::Render: failed to prepare ray-traced runtime AO resources. Falling back to default AO.\n");
            }
        }
        if (useSoftwareRayTracedDirectionalShadow || useSoftwareRayTracedReflections || useSoftwareRayTracedAmbientOcclusion) {
            m_rayTracingStats = {};
            m_rayTracingStats.hardwareSupported     = IsHardwareRayTracingSupported();
            m_rayTracingStats.instanceCount         = static_cast<uint32_t>(m_rayTracingScene.instances.size());
            m_rayTracingStats.triangleCount         = m_rayTracingScene.TriangleCount();
            m_rayTracingStats.shadowMapSize         = partialBehavior.shadowMapSize;
            m_rayTracingStats.reflectionWidth       = m_renderTargetPool.GetSWRTReflectionWidth();
            m_rayTracingStats.reflectionHeight      = m_renderTargetPool.GetSWRTReflectionHeight();
            m_rayTracingStats.shadowUpdateInterval  = partialBehavior.shadowUpdateInterval;
            m_rayTracingStats.reflectionUpdateInterval = partialBehavior.reflectionUpdateInterval;
            m_rayTracingStats.reflectionPhaseCount  = partialBehavior.reflectionPhaseCount;
            m_rayTracingStats.reflectionPhaseIndex  = m_swrtExecutor.GetReflectionPhaseIndex();
            m_rayTracingStats.reflectionResolutionScale = partialBehavior.reflectionResolutionScale;
            m_rayTracingStats.reflectionMaxRoughness = partialBehavior.reflectionMaxRoughness;
            m_rayTracingStats.reflectionMinEnergy   = partialBehavior.reflectionMinEnergy;
            m_rayTracingStats.reflectionMaxDistance = partialBehavior.reflectionMaxTraceDistance;
        }

        const bool hasLightingPass =
            std::any_of(m_passRegistry.GetPasses().begin(),
                        m_passRegistry.GetPasses().end(),
                        [](const std::shared_ptr<IRenderPass>& runtimePass) {
                            if (!runtimePass) {
                                return false;
                            }
                            const std::string_view tag = runtimePass->Tag();
                            return tag == "Lighting" || tag == "TransparentLighting";
                        });
        context.useShadowTessPath = hasLightingPass && m_settings.useTessellation;

        // Accumulated across the four cascade invocations of drawShadowItems within this frame.
        // Two questions this answers, which the GPU timestamp alone cannot:
        //  - How long the CPU spends RECORDING the shadow draws. The frame is recorded into one
        //    command list and submitted once at frame end, so recording finishes before the GPU
        //    starts and cannot stall it mid-pass; if CPU record time is far below the GPU span,
        //    the GPU span is genuine GPU work rather than starvation.
        //  - How many triangles those draws carry. Reducing cascade count lowers draw count and
        //    triangle count together, so the measured linearity in draw count is confounded and
        //    cannot by itself attribute the cost to per-draw overhead.
        FrameRenderContext::ShadowRecordStats& shadowRecordStats = context.shadowRecordStats;

        context.drawShadowItems = [this, &graphicsCommandEncoder, cmdList, frame, &shadowRecordStats](const LightSystem::ShadowPassContext& shadowContext) {
            const auto recordBegin = std::chrono::steady_clock::now();
            auto* enc = &graphicsCommandEncoder;
            // Per-cascade culling. This is the whole point of having cascades: cascade 0 covers a
            // few metres, so drawing the entire city into it is pure waste. Before this, the
            // shadow pass issued exactly meshCount * cascadeCount draws (1698 * 4 = 6792 measured),
            // i.e. no culling whatsoever.
            //
            // The near plane is deliberately ignored. An object that sits between the light and
            // the cascade box is outside the box but still casts a shadow INTO it; clipping it
            // against the near plane would make that shadow pop out of existence.
            const Culling::FrustumPlanes cascadeFrustum =
                Culling::ExtractFrustumPlanes(shadowContext.lightViewProjection);

            size_t shadowConsidered = 0;
            size_t shadowDrawn     = 0;
            // glTF MASK casters, drawn after the rest with the alpha-tested PSO (one switch per pass).
            std::vector<const SceneSubmitter::DrawItem*> alphaMaskItems;

            const auto drawShadowMesh = [&](const SceneSubmitter::DrawItem& item, const float* maskExtra0,
                                            const float* maskExtra6) {
                float objLightMVP[16];
                Mul4x4(item.model, shadowContext.lightViewProjection, objLightMVP);
                static const float kZeroExtra[4] = {};
                const RhiGpuAddress cameraCbGpu = maskExtra0
                    ? m_frameCoordinator.PushCameraCB(*frame, objLightMVP, item.model, maskExtra0, kZeroExtra,
                                                      kZeroExtra, nullptr, nullptr, nullptr, maskExtra6)
                    : m_frameCoordinator.PushCameraCB(*frame, objLightMVP, item.model);
                if (cameraCbGpu != 0) {
                    enc->SetGraphicsConstantBufferView(2, cameraCbGpu);
                }

                m_meshBuffer.Bind(enc, item.meshIndex);
                const auto& items = m_meshBuffer.Items();
                if (item.meshIndex < items.size()) {
                    const auto& it = items[item.meshIndex];
                    if (it.indexCount > 0) {
                        enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
                        shadowRecordStats.triangles += it.indexCount / 3u;
                        ++shadowRecordStats.draws;
                    } else if (it.vertexCount > 0) {
                        enc->Draw({ it.vertexCount, 1, 0, 0 });
                        shadowRecordStats.triangles += it.vertexCount / 3u;
                        ++shadowRecordStats.draws;
                    }
                }
            };

            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.transparent) {
                    continue;
                }
                if (shadowContext.dynamicVerticesOnly && !item.dynamicVertices) {
                    continue;
                }
                ++shadowConsidered;
                if (item.hasWorldBounds &&
                    !Culling::IsAabbVisibleIgnoringNearPlane(cascadeFrustum,
                                                             item.worldBoundsMin,
                                                             item.worldBoundsMax)) {
                    continue;
                }
                ++shadowDrawn;
                if (shadowContext.allowAlphaMaskPipeline && GetGBufferAlphaCutoff(item.material) > 0.0f) {
                    alphaMaskItems.push_back(&item);
                    continue;
                }
                drawShadowMesh(item, nullptr, nullptr);
            }

            if (!alphaMaskItems.empty()) {
                auto& psoCache = m_pipelineStateCache;
                cmdList->SetPipelineState(shadowContext.d16Target ? psoCache.GetShadowAlphaMaskD16PipelineState()
                                                                  : psoCache.GetShadowAlphaMaskPipelineState());
                for (const SceneSubmitter::DrawItem* maskItem : alphaMaskItems) {
                    const float extra6[4] = { 0.0f, 0.0f, GetGBufferAlphaCutoff(maskItem->material), 0.0f };
                    BindShadowAlphaMaskAlbedo(*enc, maskItem->texture);
                    drawShadowMesh(*maskItem, maskItem->material.baseColor, extra6);
                }
                // The shadow pass set the plain depth-only PSO and expects it back for the next callback.
                cmdList->SetPipelineState(shadowContext.d16Target ? psoCache.GetShadowD16PipelineState()
                                                                  : psoCache.GetShadowPipelineState());
            }

            shadowRecordStats.recordMicroseconds +=
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - recordBegin).count();

            // Reported on the last cascade so the numbers cover a whole frame's shadow recording.
            if (shadowContext.activeCascadeIndex + 1u >= shadowContext.cascadeCount) {
                static unsigned int s_shadowStatSample = 0u;
                if ((s_shadowStatSample++ % 240u) == 0u) {
                    char statLine[224];
                    std::snprintf(statLine, sizeof(statLine),
                                  "[ShadowRecord] cpuRecord=%.2fms draws=%llu tris=%llu (cpuPerDraw=%.2fus)\n",
                                  shadowRecordStats.recordMicroseconds / 1000.0,
                                  static_cast<unsigned long long>(shadowRecordStats.draws),
                                  static_cast<unsigned long long>(shadowRecordStats.triangles),
                                  shadowRecordStats.draws > 0
                                      ? (shadowRecordStats.recordMicroseconds / static_cast<double>(shadowRecordStats.draws))
                                      : 0.0);
                    DebugLog(statLine);
                }
                shadowRecordStats = FrameRenderContext::ShadowRecordStats{};
            }

            // Labelled per cascade: the four cascades have very different frustum volumes, so a
            // single merged "ShadowCascade" number cannot show which cascade dominates the cost.
            char cascadeLabel[32];
            std::snprintf(cascadeLabel, sizeof(cascadeLabel), "ShadowCascade[%u]", shadowContext.activeCascadeIndex);
            LogCullingStats(cascadeLabel, shadowConsidered, shadowDrawn);
        };
    }
}
