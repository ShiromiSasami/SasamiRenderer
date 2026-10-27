#include "Renderer/Runtime/NativeBackendFrame.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Passes/Debug/DebugProbeGridRenderPass.h"
#include "Renderer/Scene/SceneSynchronizer.h"
#include "Renderer/Scene/EnvironmentManager.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>
#include <windowsx.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "d3dx12.h"

using namespace std;

namespace SasamiEngine::Render
{
    using Math::Mul4x4;

    namespace
    {
        // ToCompatibilityDx12State / ToCompatibilityDx12Topology / D3D12CommandListRhiEncoder
        // used to be defined here as well. They now live in D3D12CommandListRhiEncoder.h with
        // external linkage so RendererDrawCallbacks.cpp and RendererPassServices.cpp can share
        // one definition; this file reaches them through Renderer.h -> FrameRenderContext.h.
        uint32_t ComputeScaledDimension(uint32_t dimension, float scale)
        {
            return std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(dimension) * scale)));
        }

        bool UsesRuntimeAmbientOcclusionMode(RendererEnums::AmbientOcclusionMode mode)
        {
            return mode == RendererEnums::AmbientOcclusionMode::RuntimeAOOnly ||
                   mode == RendererEnums::AmbientOcclusionMode::RayTracedAOOnly ||
                   mode == RendererEnums::AmbientOcclusionMode::Hybrid;
        }

        bool UsesRayTracedRuntimeAmbientOcclusion(RendererEnums::AmbientOcclusionMode mode,
                                                  RendererEnums::RuntimeAmbientOcclusionMethod method)
        {
            return mode != RendererEnums::AmbientOcclusionMode::MaterialOnly &&
                   (method == RendererEnums::RuntimeAmbientOcclusionMethod::RayTraced ||
                    mode == RendererEnums::AmbientOcclusionMode::RayTracedAOOnly);
        }

        bool UsesSsaoRuntimeAmbientOcclusion(RendererEnums::AmbientOcclusionMode mode,
                                             RendererEnums::RuntimeAmbientOcclusionMethod method)
        {
            return UsesRuntimeAmbientOcclusionMode(mode) &&
                   !UsesRayTracedRuntimeAmbientOcclusion(mode, method);
        }

        bool UsesReflectionDebugView(RendererEnums::GBufferDebugView view)
        {
            return view == RendererEnums::GBufferDebugView::ReflectionRadiance ||
                   view == RendererEnums::GBufferDebugView::ReflectionAlpha ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionHitDistance ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionComposite;
        }


    }

    void Renderer::RetireDeferredUploadBatches()
    {
        if (m_deferredUploadBatches.empty()) {
            return;
        }

        const auto newEnd = std::remove_if(m_deferredUploadBatches.begin(),
                                           m_deferredUploadBatches.end(),
                                           [this](const DeferredUploadBatch& batch) {
                                               return m_frameCoordinator.IsFenceComplete(batch.retireFenceValue);
                                           });
        m_deferredUploadBatches.erase(newEnd, m_deferredUploadBatches.end());
    }

    // EnsureEnvironmentTexturesUploaded and BuildSwrtFrameContext
    // are now implemented in EnvironmentManager and SceneSynchronizer respectively.

    RenderPassExecutionPolicy Renderer::BuildRenderPassExecutionPolicy(bool useShadowTessPath)
    {
        const SWRTExecutor::PartialBehavior partialBehavior =
            m_swrtExecutor.ResolveBehavior(m_settings.rayTracingPerformancePreset);
        RenderPassExecutionPolicy policy{};
        policy.useTessellation = m_settings.useTessellation;
        policy.useTessellationWireframe = m_settings.tessWireframeEnabled;
        policy.useTessellationDebugColors = m_settings.tessDebugColorsEnabled;
        policy.useMeshShader = m_settings.useMeshShader &&
            m_pipelineStateCache.GetMeshShaderPipelineState().Get() != nullptr &&
            m_pipelineStateCache.GetMeshShaderRootSignature().Get() != nullptr;
        policy.useShadowTessellationPath = useShadowTessPath;
        policy.useSoftwareRayTracedDirectionalShadow =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled &&
            m_renderTargetPool.GetSWRTShadowTexture().IsValid();
        // Tells LightSystem::UpdateFrameLighting (via dirColor.w) whether the SWRT directional
        // shadow texture is trustworthy this frame, so the pixel shader can fall back to fully
        // lit instead of sampling a never-rendered raster cascade (see BuildRenderPassFrameInputs,
        // which uses the identical condition to pick the SRV bound at ShadowMapTex).
        m_lightSystem.SetDirectionalShadowSourceState(
            policy.useSoftwareRayTracedDirectionalShadow,
            policy.useSoftwareRayTracedDirectionalShadow && m_swrtExecutor.IsShadowCacheValid());
        policy.useSoftwareRayTracedReflections =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled &&
            m_renderTargetPool.GetSWRTReflectionTexture().IsValid();
        const bool useScreenSpaceReflections =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            !policy.useSoftwareRayTracedReflections &&
            m_settings.rasterScreenSpaceReflectionEnabled &&
            m_renderTargetPool.GetSSRSceneColorCopyTexture().IsValid() &&
            m_renderTargetPool.GetSSRReflectionTexture().IsValid();
        policy.useScreenSpaceReflections = useScreenSpaceReflections;
        policy.reflectionMode = policy.useSoftwareRayTracedReflections ? 1.0f : 0.0f;
        policy.ssrMaxDistance = m_settings.ssrMaxDistance;
        policy.ssrThickness = m_settings.ssrThickness;
        policy.ssrStepCount = m_settings.ssrStepCount;
        policy.ssrRoughnessCutoff = m_settings.ssrRoughnessCutoff;
        policy.ssrRefineSteps = m_settings.ssrRefineSteps;
        policy.ssrEdgeFade = m_settings.ssrEdgeFade;
        policy.ssrNormalOffset = m_settings.ssrNormalOffset;
        policy.ssrIntensity = m_settings.ssrIntensity;
        policy.softwareRayTracedShadowMapSize =
            policy.useSoftwareRayTracedDirectionalShadow
                ? partialBehavior.shadowMapSize
                : 4096u;
        policy.renderWidth = static_cast<uint32_t>(std::max(0.0f, m_viewport.Width));
        policy.renderHeight = static_cast<uint32_t>(std::max(0.0f, m_viewport.Height));
        policy.iblIntensity = m_settings.iblIntensity;
        policy.gBufferDebugView = m_settings.gBufferDebugView;
        policy.renderPathMode = m_settings.renderPathMode;
        policy.vsmBlurEnabled = m_settings.vsmBlurEnabled;
        m_lightSystem.SetAoMinOcclusion(m_settings.aoMinOcclusion);
        m_lightSystem.SetAoDirectLightingStrength(m_settings.aoDirectLightingStrength);
        m_lightSystem.SetPunctualLightFalloff(m_settings.punctualLightFalloff);
        return policy;
    }

    RenderPassFrameInputs Renderer::BuildRenderPassFrameInputs(CommandList* cmdList,
                                                               IRhiCommandEncoder* commandEncoder,
                                                               RendererFrameCoordinator::FrameContext* frame,
                                                               D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                               GpuDescriptorHandle defaultAoSrv)
    {
        RenderPassFrameInputs inputs{};
        inputs.execution.cmdList = cmdList;
        inputs.execution.commandEncoder = commandEncoder;
        inputs.execution.pipelineStateCache = &m_pipelineStateCache;
        inputs.execution.srvHeap = m_srvAllocator.GetHeap();
        inputs.execution.viewport = &m_viewport;
        inputs.execution.scissorRect = &m_scissorRect;
        inputs.execution.frameCoordinator = &m_frameCoordinator;
        inputs.execution.frame = frame;

        inputs.lighting.lightSystem = &m_lightSystem;
        inputs.lighting.frameLight = frame ? &frame->light : nullptr;
        inputs.skybox = &m_skybox;
        inputs.camera.pv = m_cameraState.GetPV();
        inputs.camera.invPv = m_cameraState.GetInvPV();
        inputs.camera.pos = m_cameraState.GetPos();
        inputs.camera.right = m_cameraState.GetRight();
        inputs.camera.up = m_cameraState.GetUp();
        inputs.camera.forward = m_cameraState.GetForward();
        inputs.camera.tanHalfFovY = m_cameraState.GetTanHalfFovY();
        inputs.camera.aspectRatio = m_cameraState.GetAspectRatio();
        inputs.camera.mode = m_cameraState.GetCameraMode();
        // Same condition as the dirColor.w write in BuildRenderPassExecutionPolicy: whenever SWRT
        // directional shadow is enabled but not trustworthy this frame, the raster cascade texture
        // may never have been rendered (ShadowRenderPass replaces raster cascade rendering entirely
        // while SWRT is active, see ShadowRenderPass::Execute), so binding it here would just make
        // shadow sampling read undefined/stale data instead of failing safely. We still need a
        // validly-allocated, dimension-matching descriptor at the ShadowMapTex slot (the root
        // signature requires one even though the shader will not sample it), so lazily ensure the
        // raster shadow resources exist (without rendering into them) and bind that; the pixel
        // shader's dirColor.w == 2 branch skips sampling it entirely (see PBR_Shadow.hlsli).
        const bool swrtDirectionalShadowTrustworthy =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled &&
            m_renderTargetPool.GetSWRTShadowTexture().IsValid() &&
            m_swrtExecutor.IsShadowCacheValid();
        if (swrtDirectionalShadowTrustworthy) {
            inputs.shadow.shadowSrv = m_renderTargetPool.GetSWRTShadowSrv();
        } else {
            m_lightSystem.EnsureDirectionalShadowResources();
            inputs.shadow.shadowSrv = m_lightSystem.GetShadowSrv();
        }
        inputs.shadow.spotShadowSrv = m_lightSystem.GetSpotShadowSrv();
        inputs.shadow.pointShadowSrv = m_lightSystem.GetPointShadowSrv();
        inputs.shadow.vsmSrv = m_lightSystem.GetVsmSrv();
        inputs.lighting.lightSrvTable = frame ? frame->light.lightSrvTable : GpuDescriptorHandle{};
        inputs.lighting.iblSrvTable = m_skybox.GetIblSrvTable();
        inputs.lighting.iblSpecularSrvTable = m_skybox.GetIblSpecularSrvTable();
        if (m_probeGrid.IsInitialized()) {
            inputs.lighting.giProbeGridCbGpu = m_probeGrid.GetProbeGridCbGpuAddress();
            inputs.lighting.giProbeDataGpu = m_probeGrid.GetProbeDataGpuVA();
        }
        inputs.ao.aoSrv = defaultAoSrv;
        const bool useSwrtReflection =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled &&
            m_renderTargetPool.GetSWRTReflectionTexture().IsValid();
        inputs.reflectionSrv = useSwrtReflection
            ? m_renderTargetPool.GetSWRTReflectionSrv()
            : m_nullTextureSrv;
        inputs.transmissionSceneColorSrv = m_renderTargetPool.GetTransmissionSceneColorCopyTexture().IsValid()
            ? m_renderTargetPool.GetTransmissionSceneColorCopySrv()
            : m_nullTextureSrv;
        inputs.transparentBackfaceDistanceSrv = m_renderTargetPool.GetTransparentBackfaceDistanceTexture().IsValid()
            ? m_renderTargetPool.GetTransparentBackfaceDistanceSrv()
            : m_nullTextureSrv;
        inputs.transparentOitAccumSrv = m_renderTargetPool.GetTransparentOitAccumTexture().IsValid()
            ? m_renderTargetPool.GetTransparentOitAccumSrv()
            : m_nullTextureSrv;
        inputs.transparentOitRevealageSrv = m_renderTargetPool.GetTransparentOitRevealageTexture().IsValid()
            ? m_renderTargetPool.GetTransparentOitRevealageSrv()
            : m_nullTextureSrv;
        inputs.lighting.lightCbGpu = lightCbGpu;
        inputs.sceneTimeSec = m_sceneTime;

        // GBuffer SRV inputs for deferred/composition passes.
        inputs.gbuffer.albedoSrv   = m_renderTargetPool.GetGBufferAlbedoSrv();
        inputs.gbuffer.materialSrv = m_renderTargetPool.GetGBufferMaterialSrv();
        inputs.gbuffer.emissiveSrv = m_renderTargetPool.GetGBufferEmissiveSrv();
        inputs.gbuffer.specularWorkflowSrv = m_renderTargetPool.GetGBufferSpecularWorkflowSrv();
        inputs.gbuffer.albedoResource =
            m_renderTargetPool.GetGBufferAlbedo().IsValid() ? m_renderTargetPool.GetGBufferAlbedo().Get() : nullptr;
        inputs.gbuffer.normalResource =
            m_renderTargetPool.GetGBufferNormal().IsValid() ? m_renderTargetPool.GetGBufferNormal().Get() : nullptr;
        inputs.gbuffer.materialResource =
            m_renderTargetPool.GetGBufferMaterial().IsValid() ? m_renderTargetPool.GetGBufferMaterial().Get() : nullptr;
        inputs.gbuffer.emissiveResource =
            m_renderTargetPool.GetGBufferEmissive().IsValid() ? m_renderTargetPool.GetGBufferEmissive().Get() : nullptr;
        inputs.gbuffer.specularWorkflowResource =
            m_renderTargetPool.GetGBufferSpecularWorkflow().IsValid() ? m_renderTargetPool.GetGBufferSpecularWorkflow().Get() : nullptr;

        // Runtime AO resources. SSAO and RTAO share the same AO consumption slot in lighting.
        inputs.gbuffer.depthSrv = m_renderTargetPool.GetDepthSrv();
        inputs.gbuffer.normalSrv = m_renderTargetPool.GetGBufferNormal().IsValid()
            ? m_renderTargetPool.GetGBufferNormalSrv()
            : m_nullTextureSrv;
        inputs.gbuffer.depthResource = m_renderTargetPool.GetDepth().IsValid() ? m_renderTargetPool.GetDepth().Get() : nullptr;
        if (m_renderTargetPool.GetSceneColorTexture().IsValid() &&
            m_renderTargetPool.GetSSRSceneColorCopyTexture().IsValid() &&
            m_renderTargetPool.GetSSRReflectionTexture().IsValid()) {
            inputs.ssr.sceneColorResource = m_renderTargetPool.GetSceneColorTexture().Get();
            inputs.ssr.sceneColorCopyResource = m_renderTargetPool.GetSSRSceneColorCopyTexture().Get();
            inputs.ssr.reflectionResource = m_renderTargetPool.GetSSRReflectionTexture().Get();
            inputs.ssr.sceneColorRtv = m_renderTargetPool.GetSceneColorRtv();
            inputs.ssr.sceneColorCopySrv = m_renderTargetPool.GetSSRSceneColorCopySrv();
            inputs.ssr.reflectionSrv = m_renderTargetPool.GetSSRReflectionSrv();
            inputs.ssr.reflectionUav = m_renderTargetPool.GetSSRReflectionUav();
        }
        // GetEffectiveAmbientOcclusionMode folds runtimeAoEnabled in once, so the predicates and the
        // switch below cannot disagree about what "runtime AO off" means. The RayTracedAOOnly arm of
        // that switch used to read the raw setting and bound the SWRT AO texture even with the master
        // switch off, which is the same hole the native path had at RendererNativeFrame.cpp.
        const RendererEnums::AmbientOcclusionMode effectiveAoMode = GetEffectiveAmbientOcclusionMode();
        const bool usesSsaoAo =
            UsesSsaoRuntimeAmbientOcclusion(effectiveAoMode, m_settings.runtimeAoMethod);
        const bool usesRayTracedAo =
            UsesRayTracedRuntimeAmbientOcclusion(effectiveAoMode, m_settings.runtimeAoMethod);
        inputs.ao.ssaoRtv = m_renderTargetPool.GetSSAORtv();
        inputs.ao.ssaoResource =
            (usesSsaoAo && m_renderTargetPool.GetSSAOTexture().IsValid()) ? m_renderTargetPool.GetSSAOTexture().Get() : nullptr;
        inputs.ao.ssaoRawSrv =
            (usesSsaoAo && m_renderTargetPool.GetSSAOTexture().IsValid()) ? m_renderTargetPool.GetSSAOSrv() : m_nullTextureSrv;
        inputs.ao.ssaoBlurRtv = m_renderTargetPool.GetSSAOBlurRtv();
        inputs.ao.ssaoBlurResource =
            (usesSsaoAo && m_renderTargetPool.GetSSAOBlurTexture().IsValid()) ? m_renderTargetPool.GetSSAOBlurTexture().Get() : nullptr;
        switch (effectiveAoMode) {
        case RendererEnums::AmbientOcclusionMode::MaterialOnly:
            inputs.ao.screenSpaceAoSrv = defaultAoSrv;
            break;
        case RendererEnums::AmbientOcclusionMode::RuntimeAOOnly:
        case RendererEnums::AmbientOcclusionMode::Hybrid:
            if (usesRayTracedAo) {
                inputs.ao.screenSpaceAoSrv =
                    m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                        ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                        : defaultAoSrv;
            } else if (usesSsaoAo) {
                inputs.ao.screenSpaceAoSrv =
                    m_renderTargetPool.GetSSAOBlurTexture().IsValid() ? m_renderTargetPool.GetSSAOBlurSrv()
                    : m_renderTargetPool.GetSSAOTexture().IsValid() ? m_renderTargetPool.GetSSAOSrv()
                    : defaultAoSrv;
            } else {
                // runtimeAoEnabled is false: the SSAO pass above is a no-op and never refreshes
                // SSAOBlur/SSAO, so binding those textures here would keep showing whatever AO was
                // baked into them the last time Runtime AO was on. Fall back to the flat white SRV
                // so disabling Runtime AO actually removes AO instead of freezing the last frame.
                inputs.ao.screenSpaceAoSrv = defaultAoSrv;
            }
            break;
        case RendererEnums::AmbientOcclusionMode::RayTracedAOOnly:
            inputs.ao.screenSpaceAoSrv =
                m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                    ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                    : defaultAoSrv;
            break;
        default:
            inputs.ao.screenSpaceAoSrv = defaultAoSrv;
            break;
        }

        // Push Runtime AO CB for the SSAO generator. RTAO uses the same tuning fields in SWRTExecutor.
        if (frame && usesSsaoAo) {
            const float ssaoExtra0[4] = {
                m_settings.runtimeAoRadius,
                m_settings.runtimeAoBias,
                m_settings.runtimeAoIntensity,
                m_settings.runtimeAoThickness
            };
            const float ssaoExtra1[4] = {
                m_viewport.Width,
                m_viewport.Height,
                (m_viewport.Width > 0.0f) ? 1.0f / m_viewport.Width : 0.0f,
                (m_viewport.Height > 0.0f) ? 1.0f / m_viewport.Height : 0.0f,
            };
            const float ssaoExtra2[4] = {
                m_cameraState.GetPos()[0],
                m_cameraState.GetPos()[1],
                m_cameraState.GetPos()[2],
                static_cast<float>(m_settings.runtimeAoQuality)
            };
            inputs.ao.ssaoCbGpu = m_frameCoordinator.PushCameraCB(
                *frame, m_cameraState.GetPV(), m_cameraState.GetInvPV(), ssaoExtra0, ssaoExtra1, ssaoExtra2);
        }

        return inputs;
    }

    RenderPassExecutionServices Renderer::BuildRenderPassExecutionServices(
        const DrawSceneItemsCallback& drawItems,
        const DrawShadowItemsCallback& drawShadowItems,
        bool useMeshShader)
    {
        RenderPassExecutionServices services{};
        // The cluster path (drawClusterItems) replaces the VS OpaqueGBuffer draw for static
        // opaque items when active, so the VS loop must not also draw them -- otherwise every
        // static opaque item would be rasterized twice. Transparent items always go through the
        // VS path; the cluster path only covers static opaque geometry. Dynamic-vertex items have no
        // clusters (SceneSubmitter leaves their DAGs empty), so the VS loop still draws those.
        services.drawOpaqueItems = [drawItems, useMeshShader]() { drawItems(false, useMeshShader); };
        services.drawOpaqueItemsVertexPath = [drawItems]() { drawItems(false, false); };
        services.drawTransparentItems = [drawItems]() { drawItems(true, false); };
        services.drawShadowItems = drawShadowItems;
        services.copySceneColorForTransmission = []() { return true; };
        return services;
    }


    Renderer::Renderer()
        : m_sceneSynchronizer(m_sceneSubmitter,
                              m_cameraState,
                              m_rayTracingScene,
                              m_swrtExecutor,
                              m_drawCommandBuilder,
                              m_deltaTime,
                              m_viewport)
        , m_environmentManager(m_skybox, m_swrtExecutor)
    {
        ScopedPerfTimer perfTimer("Renderer::Renderer");

        m_renderPassBuilderCatalog = RenderPassBuilderCatalog::CreateDefault();

        // The device does not exist yet, so nothing here can report real capabilities. These are
        // placeholders that assume everything is supported, which is only safe because the passes
        // built from them do not gate on capabilities -- HardwareRayTracingPassBuilder is the one
        // builder that does, and it is deliberately NOT built here. InitializeCore builds it once
        // the device can answer honestly; until then m_rayTracingRenderPass stays null.
        RenderPassBuildContext buildContext{};
        buildContext.capabilities.supportsFeatureRenderPasses = true;
        buildContext.capabilities.supportsD3D12CompatibilitySurface = true;
        buildContext.capabilities.supportsHardwareRayTracing = true;
        buildContext.capabilities.supportsComputeQueue = true;
        buildContext.capabilities.supportsSwapChain = true;

        auto builtins = m_renderPassBuilderCatalog.BuildBuiltinSequencePasses(buildContext);
        m_shadowRenderPass = std::dynamic_pointer_cast<ShadowRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Shadow)]);
        m_opaqueGBufferRenderPass = std::dynamic_pointer_cast<GBufferRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::OpaqueGBuffer)]);
        m_lightingRenderPass = std::dynamic_pointer_cast<DeferredLightingRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Lighting)]);
        m_transparentLightingRenderPass = std::dynamic_pointer_cast<TransparentLightingRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentLighting)]);
        m_transparentBackfaceDistanceRenderPass = std::dynamic_pointer_cast<TransparentBackfaceDistanceRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentBackfaceDistance)]);
        m_transparentCompositeRenderPass = std::dynamic_pointer_cast<TransparentCompositeRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentComposite)]);
        m_transparentSceneColorCopyRenderPass = std::dynamic_pointer_cast<TransparentSceneColorCopyRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentSceneColorCopy)]);
        m_skyboxRenderPass = std::dynamic_pointer_cast<SkyboxRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Skybox)]);
        m_postProcessRenderPass = std::dynamic_pointer_cast<PostProcessRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::PostProcess)]);
        m_ssaoRenderPass = std::dynamic_pointer_cast<SSAORenderPass>(
            builtins[static_cast<size_t>(RenderPassType::RuntimeAO)]);
        m_ssaoBlurRenderPass = std::dynamic_pointer_cast<SSAOBlurRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::RuntimeAOBlur)]);
        m_screenSpaceReflectionRenderPass = std::dynamic_pointer_cast<ScreenSpaceReflectionRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::ScreenSpaceReflection)]);
        m_screenSpaceReflectionCompositeRenderPass = std::dynamic_pointer_cast<ScreenSpaceReflectionCompositeRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::ScreenSpaceReflectionComposite)]);
        m_softwareReflectionRenderPass = std::dynamic_pointer_cast<SoftwareReflectionRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::SoftwareReflection)]);
        m_softwareReflectionCompositeRenderPass = std::dynamic_pointer_cast<SoftwareReflectionCompositeRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::SoftwareReflectionComposite)]);
        m_proceduralSkyRenderPass = std::dynamic_pointer_cast<ProceduralSkyRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::ProceduralSky)]);
        m_volumetricCloudRenderPass = std::dynamic_pointer_cast<VolumetricCloudRenderPass>(
            m_renderPassBuilderCatalog.Build(RenderPassBuilderId::VolumetricCloud, buildContext));
        // Not part of the catalog: the virtual-geometry cluster path has no builder-specific
        // construction inputs (see MeshShaderRenderPass.h), so it is built directly here.
        m_meshShaderRenderPass = std::make_shared<MeshShaderRenderPass>();
        m_debugVisualization.Bind(
            std::dynamic_pointer_cast<DebugProbeGridRenderPass>(
                m_renderPassBuilderCatalog.Build(RenderPassBuilderId::DebugProbeGrid, buildContext)),
            [this] { EnsureDebugProbeGridPassInserted(); });
        m_fluidSurfaceRenderPass = std::dynamic_pointer_cast<FluidSurfaceRenderPass>(
            m_renderPassBuilderCatalog.Build(RenderPassBuilderId::FluidSurface, buildContext));
        m_particleRenderPass = std::dynamic_pointer_cast<ParticleRenderPass>(
            m_renderPassBuilderCatalog.Build(RenderPassBuilderId::Particles, buildContext));
        m_crowdRenderPass = std::make_shared<CrowdRenderPass>();
        m_crowdRenderPass->SetCrowdSystem(&m_crowdSystem);
        m_wireframeOverlayRenderPass = std::make_shared<WireframeOverlayRenderPass>();

        m_passRegistry.SetBuiltinPasses(builtins);

        m_passRegistry.UseDefaultRenderNodePreset();
    }

    Renderer::~Renderer()
    {
        WaitForGPU();

        if (m_computeFrameFenceEvent) {
            CloseHandle(m_computeFrameFenceEvent);
            m_computeFrameFenceEvent = nullptr;
        }

        m_crowdSystem.Shutdown();
        if (m_particleVolumeRenderPass) m_particleVolumeRenderPass->Shutdown();

        m_frameCoordinator.Shutdown(m_lightSystem);

        m_skybox.Shutdown();

        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
    }

    // Renderer::Initialize ↁERendererInitialization.cpp

    // Back buffer management is now delegated to RenderTargetPool.

    void Renderer::WaitForGPU()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
    }

    bool Renderer::IsHardwareRayTracingSupported() const
    {
        return m_device ? m_device->GetCapabilities().supportsHardwareRayTracing : false;
    }

    const RhiBackendCapabilities& Renderer::GetBackendCapabilities() const
    {
        static const RhiBackendCapabilities kNoCapabilities{};
        return m_device ? m_device->GetCapabilities() : kNoCapabilities;
    }

    void Renderer::SetRenderPathMode(RenderPathMode mode)
    {
        if (mode != RenderPathMode::Raster &&
            mode != RenderPathMode::HardwareRayTracing) {
            mode = RenderPathMode::Raster;
        }

        if (mode == RenderPathMode::HardwareRayTracing &&
            !IsHardwareRayTracingSupported()) {
            mode = RenderPathMode::Raster;
        }

        m_settings.renderPathMode = mode;

        if (m_settings.renderPathMode != RenderPathMode::Raster) {
            m_settings.gBufferDebugView = GBufferDebugView::FinalLit;
        }

        EnsureVolumetricCloudPassInserted();
        EnsureMeshShaderPassInserted();
        EnsureCrowdPassInserted();
        EnsureWireframeOverlayPassInserted();
    }

    void Renderer::SetVolumetricCloudEnabled(bool e)
    {
        m_settings.volumetricCloudEnabled = e;
        if (m_volumetricCloudRenderPass) {
            m_volumetricCloudRenderPass->SetEnabled(e);
        }
        if (e) {
            EnsureVolumetricCloudPassInserted();
        }
    }

    void Renderer::SetDirectionalLightSettings(const DirectionalLightSettings& settings)
    {
        m_lightSystem.SetDirectionalLightSettings(settings);
    }

    Renderer::DirectionalLightSettings Renderer::GetDirectionalLightSettings() const
    {
        return m_lightSystem.GetDirectionalLightSettings();
    }

    bool Renderer::GetOverlayDeviceContext(RhiOverlayDeviceContext& context) const
    {
        return m_device && m_device->GetOverlayDeviceContext(context);
    }

    bool Renderer::SupportsUIOverlay() const
    {
        if (SupportsD3D12OverlayRendering()) {
            return true;
        }
        RhiOverlayDeviceContext context{};
        return GetOverlayDeviceContext(context);
    }

    void Renderer::ResizeViewport(UINT width, UINT height)
    {
        if (width == 0 || height == 0 || !m_device) {
            return;
        }

        if (UsesNativeBackendFrame(*m_device)) {
            m_device->WaitForGPU();
            m_viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
            m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
            if (!m_device->ResizeBackendSwapChain(width, height)) {
                DebugLog("Renderer::ResizeViewport: native backend swapchain resize failed; viewport state was updated only.\n");
            }
            return;
        }

        m_device->WaitForGPU();
        m_frameCoordinator.ResetFrameFenceValues();

        m_renderTargetPool.ReleaseBackBuffers();

        // render.hdr / RenderSettings::enableHdrOutput only take effect here, at the next resize,
        // rather than the instant the setting changes: switching the swap chain's format and
        // colour space needs ResizeBuffers + SetColorSpace1, and this is the one place in the
        // non-native (DX12 raster) path that already tears down and recreates the swap chain's
        // buffers. m_device->SupportsHdr10Output() also folds in the display capability check
        // (Dx12GraphicsDeviceInit.cpp), so an unsupported display silently stays on
        // DXGI_FORMAT_R8G8B8A8_UNORM even with the setting on.
        const bool wantHdrOutput = m_settings.enableHdrOutput && m_device->SupportsHdr10Output();
        const DXGI_FORMAT swapChainFormat =
            wantHdrOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

        HRESULT hr = m_device->GetSwapChain()->ResizeBuffers(GetBackBufferCount(), width, height, swapChainFormat, 0);
        if (FAILED(hr)) {
            DebugLog("ResizeBuffers failed\n");
            return;
        }

        // SetHdr10ColorSpaceEnabled re-checks swap-chain-level colour-space support
        // (CheckColorSpaceSupport) before calling SetColorSpace1, and falls back to SDR colour
        // space if that check fails even though the format above already switched. m_hdrOutputActive
        // records what actually happened so ToneMapSceneColor's PQ-encode branch only runs when the
        // backbuffer really is DXGI_FORMAT_R10G10B10A2_UNORM in the HDR10 colour space.
        //
        // Skipped entirely (no SetColorSpace1 call at all) when HDR was never requested and is not
        // currently active, so an ordinary window resize with the feature untouched stays exactly
        // as before this feature existed.
        if (wantHdrOutput || m_hdrOutputActive) {
            m_hdrOutputActive = m_device->SetHdr10ColorSpaceEnabled(wantHdrOutput);
        }

        if (!m_renderTargetPool.InitializeBackBuffers(*m_device, m_device->GetSwapChain(), GetBackBufferCount())) {
            DebugLog("Back buffer target rebuild failed\n");
            return;
        }

        m_viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

        m_renderTargetPool.OnResize(*m_device, width, height);
        m_sceneColorHistoryValid = false;

        // Recreate SSAO resources at new size (OnResize resets them, EnsureSSAO recreates)
        if (!m_renderTargetPool.EnsureSSAO(*m_device, width, height)) {
            DebugLog("SSAO resource resize failed\n");
        }

        if (!m_renderTargetPool.EnsureGBuffer(*m_device, width, height)) {
            DebugLog("GBuffer resource resize failed\n");
        }

        if (!m_renderTargetPool.EnsureRayTracingOutput(*m_device, width, height)) {
            DebugLog("Ray tracing resource resize failed\n");
        }
        m_swrtExecutor.InvalidateCache();
    }

    void Renderer::RefreshEnvironmentAssets()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
        m_environmentManager.RefreshEnvironmentAssets();
    }

    void Renderer::SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height)
    {
        m_environmentManager.SetSkyboxHdrEquirectData(std::move(pixels), width, height);
    }

    void Renderer::AdoptPregeneratedIbl(IblSystem::GeneratedIblData&& data)
    {
        m_environmentManager.AdoptPregeneratedIblData(std::move(data));
        m_readyState.skyboxIblReady.store(true, std::memory_order_release);
    }

    void Renderer::SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height)
    {
        m_environmentManager.SetSkyboxLdrEquirectData(std::move(pixels), width, height);
    }

    void Renderer::SetSkyboxLdrCubemapFacesData(std::vector<std::vector<uint8_t>> facePixels, UINT width, UINT height)
    {
        m_environmentManager.SetSkyboxLdrCubemapFacesData(std::move(facePixels), width, height);
    }

    void Renderer::SetSkyboxLoadFormat(SkyboxLoadFormat format)
    {
        m_environmentManager.SetSkyboxLoadFormat(format);
    }


    void Renderer::UpdateCameraCB(const RenderCameraProxy* camera)
    {
        m_sceneSynchronizer.UpdateCameraCB(camera);
    }

    void Renderer::SubmitRenderProxies(std::vector<RenderProxy>&& proxies)
    {
        m_sceneSynchronizer.SubmitRenderProxies(std::move(proxies));
    }

    void Renderer::SubmitSkinnedRenderProxies(std::vector<SkinnedRenderProxy>&& proxies)
    {
        if (!m_device) {
            return;
        }
        if (UsesNativeBackendFrame(*m_device)) {
            m_sceneSubmitter.SubmitSkinnedRenderProxies(std::move(proxies));
            return;
        }
        const UINT backIndex = m_device->GetSwapChain()->GetCurrentBackBufferIndex();
        auto* frame = m_frameCoordinator.GetFrameContext(backIndex);
        if (!frame) return;
        m_sceneSubmitter.SubmitSkinnedRenderProxies(std::move(proxies), &m_frameCoordinator, frame);
    }

    void Renderer::ClearSubmittedRenderProxies()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
        m_sceneSynchronizer.ClearSubmittedRenderProxies();
        m_sceneSubmitter.ClearSkinnedRenderProxies();
    }

    void Renderer::ClearRenderObjects()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
        m_sceneSynchronizer.ClearRenderObjects();
        m_sceneSubmitter.ClearSkinnedRenderProxies();
    }

    bool Renderer::UpdateStaticMeshVertices(size_t meshIndex, const Vertex* vertices, size_t count)
    {
        const auto& items = m_meshBuffer.Items();
        if (!m_device || !vertices || meshIndex >= items.size()) {
            return false;
        }
        const MeshBuffer::GPUItem& item = items[meshIndex];
        if (!item.rhiVb.IsValid() || item.vertexCount != count) {
            return false;
        }
        return m_device->UpdateRhiBuffer(item.rhiVb, 0, vertices, sizeof(Vertex) * count);
    }

    bool Renderer::UpdateStaticMeshDynamicVertices(size_t meshIndex, const Vertex* vertices, size_t vertexCount,
                                                   size_t firstVertex, size_t count, size_t* writtenVertices)
    {
        if (!m_device) {
            return false;
        }
        // One more copy than frames in flight: BeginFrame waits for the frame GetBackBufferCount() back.
        return m_meshBuffer.WriteDynamicVertices(meshIndex, vertices, vertexCount, firstVertex, count,
                                                 GetBackBufferCount() + 1, m_renderCallCounter, writtenVertices);
    }

    bool Renderer::SetStaticMeshDrawIndexCount(size_t meshIndex, size_t indexCount)
    {
        if (indexCount > UINT32_MAX) {
            return false;
        }
        return m_meshBuffer.SetDrawIndexCount(meshIndex, static_cast<UINT>(indexCount));
    }
}
