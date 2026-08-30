#pragma once
#include "Renderer/Scene/SceneSynchronizer.h"
#include "Renderer/Scene/EnvironmentManager.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Config/RayTracingStats.h"
#include "Renderer/Config/RenderFeatureSettings.h"
#include "Renderer/Resources/RenderTargetPool.h"
#include "Renderer/RayTracing/SWRTExecutor.h"
#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Frame/RenderFrameOrchestrator.h"
#include "Renderer/Passes/Core/RenderPassBuilder.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/RenderPassConstants.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Resources/SrvDescriptorAllocator.h"
#include "Renderer/Scene/CameraState.h"
#include "Renderer/Passes/Core/RenderPassRegistry.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/SkinnedMeshBuffer.h"
#include "Renderer/Scene/DrawCommandBuilder.h"
#include "Renderer/Passes/Geometry/ShadowRenderPass.h"
#include "Renderer/Passes/Geometry/GBufferRenderPass.h"
#include "Renderer/Passes/Lighting/DeferredLightingRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentLightingRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentBackfaceDistanceRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentCompositeRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentSceneColorCopyRenderPass.h"
#include "Renderer/Passes/Sky/SkyboxRenderPass.h"
#include "Renderer/Passes/PostProcess/PostProcessRenderPass.h"
#include "Renderer/Passes/RayTracing/RayTracingRenderPass.h"
#include "Renderer/Passes/Lighting/SSAORenderPass.h"
#include "Renderer/Passes/Reflections/ScreenSpaceReflectionRenderPass.h"
#include "Renderer/Passes/Reflections/ScreenSpaceReflectionCompositeRenderPass.h"
#include "Renderer/Passes/Reflections/SoftwareReflectionCompositeRenderPass.h"
#include "Renderer/Passes/Reflections/SoftwareReflectionRenderPass.h"
#include "Renderer/Passes/Sky/ProceduralSkyRenderPass.h"
#include "Renderer/Passes/Sky/VolumetricCloudRenderPass.h"
#include "Renderer/Passes/Debug/DebugVisualizationSystem.h"
#include "Renderer/Passes/Fluid/FluidSurfaceRenderPass.h"
#include "Renderer/Fluid/FluidHeightfieldSim.h"
#include "Renderer/Passes/Particles/ParticleRenderPass.h"
#include "Renderer/Particles/ParticleSystem.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/RayTracing/DxrRayTracer.h"
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include "Renderer/GI/IrradianceProbeGrid.h"
#include "Renderer/Scene/Skybox.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/Runtime/RendererReadyState.h"
#include "Renderer/Capture/BackBufferCapture.h"
#include "Renderer/Profiling/GpuTimestampProfiler.h"
#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SasamiRenderer
{
    class InitTaskScheduler;

    class Renderer
    {
    public:

#pragma region using define

        using GBufferDebugView = RendererEnums::GBufferDebugView;
        using RenderPathMode = RendererEnums::RenderPathMode;
        using RayTracingPerformancePreset = RendererEnums::RayTracingPerformancePreset;
        using RayTracingQualityTier = RendererEnums::RayTracingQualityTier;
        using RuntimeAmbientOcclusionMethod = RendererEnums::RuntimeAmbientOcclusionMethod;
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;
        using RenderPassType = RendererEnums::RenderPassType;

        using DirectionalLightSettings = LightSystem::DirectionalLightSettings;
        using PointLight = LightSystem::PointLight;
        using SpotLight = LightSystem::SpotLight;

        using RenderProxy        = SasamiRenderer::RenderProxy;
        using SkinnedRenderProxy = SasamiRenderer::SkinnedRenderProxy;

        using OverlayRenderCallback = std::function<void(CommandList&, CpuDescriptorHandle)>;
        using PhaseCompletionMode = RenderGraph::PhaseCompletionMode;
        using PhaseCompletionCallback = std::function<bool(const RenderPassContextView&)>;

        using RayTracingStats = SasamiRenderer::RayTracingStats;

        using PassHandle = RenderPassRegistry::PassHandle;
        using NodeHandle = RenderPassRegistry::NodeHandle;

        enum class GIBakeState
        {
            Idle,
            Baking,
            Completed,
            Continuous,
            WaitingForProbeGrid,
            WaitingForBvh,
            Failed
        };

        struct GIBakeStatus
        {
            struct LogEntry
            {
                uint32_t sequence = 0u;
                uint32_t bakeFrame = 0u;
                GIBakeState state = GIBakeState::Idle;
                char phase[40] = {};
                char message[176] = {};
            };

            static constexpr uint32_t kLogCapacity = 24u;

            GIBakeState state = GIBakeState::Idle;
            bool requested = false;
            float progress = 0.0f;
            uint32_t completedProbes = 0u;
            uint32_t totalProbes = 0u;
            uint32_t probesPerStep = 0u;
            uint32_t estimatedFramesRemaining = 0u;
            uint32_t stalledFrames = 0u;
            uint32_t bvhMissingMask = 0u;
            uint32_t sceneInstances = 0u;
            uint32_t sceneTriangles = 0u;
            uint64_t sceneGeometryVersion = 0u;
            uint64_t sceneMaterialVersion = 0u;
            uint64_t sceneInstanceVersion = 0u;
            char currentPhase[80] = {};
            std::array<LogEntry, kLogCapacity> logEntries{};
            uint32_t logCount = 0u;
        };

        static constexpr uint32_t GI_BVH_MISSING_SWRT_NOT_INITIALIZED = GpuSoftwareRayTracer::BvhGpuAddresses::MISSING_SWRT_NOT_INITIALIZED;
        static constexpr uint32_t GI_BVH_MISSING_BVH_NODES            = GpuSoftwareRayTracer::BvhGpuAddresses::MISSING_BVH_NODES;
        static constexpr uint32_t GI_BVH_MISSING_TRIANGLES            = GpuSoftwareRayTracer::BvhGpuAddresses::MISSING_TRIANGLES;
        static constexpr uint32_t GI_BVH_MISSING_MESH_INFO            = GpuSoftwareRayTracer::BvhGpuAddresses::MISSING_MESH_INFO;
        static constexpr uint32_t GI_BVH_MISSING_INSTANCES            = GpuSoftwareRayTracer::BvhGpuAddresses::MISSING_INSTANCES;
        static constexpr uint32_t GI_BVH_MISSING_TLAS_NODES           = GpuSoftwareRayTracer::BvhGpuAddresses::MISSING_TLAS_NODES;
        static constexpr uint32_t GI_BVH_MISSING_MATERIALS            = GpuSoftwareRayTracer::BvhGpuAddresses::MISSING_MATERIALS;

#pragma endregion

        Renderer();
        ~Renderer();

        bool Initialize(HWND hWnd, UINT width, UINT height);

        // ---- Progressive boot (StartupCoordinator-driven) ----
        // Initialize() above remains the synchronous composite (core + frame infra +
        // finalize + all deferred tasks inline) for runtime backend switching and tools.
        //
        // Device, backbuffers, SRV heap, light system, scene submitter, the full raster
        // shader/PSO cache and builtin pass registration. Main thread (~1s warm). No
        // frame infrastructure yet -- rendering is not possible until the two calls below.
        bool InitializeCore(HWND hWnd, UINT width, UINT height);
        // RendererFrameCoordinator init (allocators/fences/command lists). Safe to call
        // from a JobSystem worker when GetBackendCapabilities().supportsThreadedResourceCreation;
        // this keeps the D3D12 debug-layer/GBV first-command-list stall off the message pump.
        bool InitializeFrameInfrastructure();
        // Main thread, after InitializeFrameInfrastructure: render target pool, default
        // material textures (deferred upload batch), compute command resources. Sets
        // coreReady/rasterReady on success.
        bool FinalizeCoreInitialization();
        // Registers every remaining init step (SWRT, GI probe grid, debug visualization,
        // fluid sim+pass, particles+pass, DXR) as scheduler tasks; each task publishes its
        // RendererReadyState flag and inserts its render pass on completion.
        void RegisterDeferredInitTasks(InitTaskScheduler& scheduler);
        // Minimal presentable frame during boot: acquire, clear, optional overlay
        // (loading UI), present. Requires FinalizeCoreInitialization.
        void RenderBootFrame(const OverlayRenderCallback& overlay = {});
        const RendererReadyState& GetReadyState() const { return m_readyState; }
        RendererReadyState& GetReadyStateMutable() { return m_readyState; }

        void Render(const OverlayRenderCallback& overlay = {});
        void WaitForGPU();

        // --- Debug screenshot -------------------------------------------------------
        // Queues a capture of the next presented frame. The copy is recorded inside the
        // frame that is already being built, so the result only becomes available after
        // that frame has been submitted -- poll ConsumeScreenshotResult for it.
        void RequestScreenshot(const std::wstring& path);
        bool HasPendingScreenshotRequest() const { return m_screenshotRequested; }
        // Returns true and fills `outMessage` once a queued capture has finished (either
        // way), clearing the result. Returns false while a capture is still in flight or
        // when nothing was ever requested.
        bool ConsumeScreenshotResult(std::string& outMessage);

        // --- GPU timing -------------------------------------------------------------
        // Per-pass GPU milliseconds for the most recent frame whose results have been read
        // back (kFrameLatency frames behind the frame currently being recorded). Empty
        // until enough frames have been presented, or if the profiler failed to initialize.
        const std::vector<GpuTimestampProfiler::ScopeResult>& GetGpuPassTimings() const
        {
            return m_gpuTimestampProfiler.GetResults();
        }
        // False means the profiler never initialized, so timings will never arrive; an empty
        // timing list while this is true just means the latency window has not filled yet.
        bool IsGpuTimingReady() const { return m_gpuTimestampProfiler.IsReady(); }
        // Diagnostic counters; see GpuTimestampProfiler for what each distinguishes.
        uint32_t GetGpuTimingScopesThisFrame() const { return m_gpuTimestampProfiler.GetScopesThisFrame(); }
        uint32_t GetGpuTimingLastResolvedScopeCount() const { return m_gpuTimestampProfiler.GetLastResolvedScopeCount(); }
        uint64_t GetGpuTimingLastUpdateRequestedFrame() const { return m_gpuTimestampProfiler.GetLastUpdateRequestedFrame(); }
        uint64_t GetGpuTimingLastUpdateSlotFrame() const { return m_gpuTimestampProfiler.GetLastUpdateSlotFrame(); }
        bool     GetGpuTimingLastUpdateSlotValid() const { return m_gpuTimestampProfiler.GetLastUpdateSlotValid(); }
        uint64_t GetGpuTimingFrameCounter() const { return m_gpuProfilerFrameCounter; }

        void UpdateCameraCB(const RenderCameraProxy* camera);
        void SubmitRenderProxies(std::vector<RenderProxy>&& proxies);
        void SubmitSkinnedRenderProxies(std::vector<SkinnedRenderProxy>&& proxies);
        void ClearSubmittedRenderProxies();
        void ClearRenderObjects();
        void SetDeltaTime(float dt) { m_deltaTime = dt; }
        void SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height);
        // Adopts worker-pregenerated IBL data (equirect pixels are set separately via
        // SetSkyboxHdrEquirectData). Sets skyboxIblReady. Main thread.
        void AdoptPregeneratedIbl(IblSystem::GeneratedIblData&& data);
        void SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height);
        void SetSkyboxLdrCubemapFacesData(std::vector<std::vector<uint8_t>> facePixels, UINT width, UINT height);
        void SetSkyboxLoadFormat(SkyboxLoadFormat format);
        bool GetShowDirectionalLightOnSkybox() const { return m_skybox.IsDirectionalLightMarkerEnabled(); }
        void SetShowDirectionalLightOnSkybox(bool enabled) { m_skybox.SetDirectionalLightMarkerEnabled(enabled); }
        float GetDirectionalLightOnSkyboxAngularRadius() const { return m_skybox.GetDirectionalLightMarkerAngularRadius(); }
        void SetDirectionalLightOnSkyboxAngularRadius(float radians) { m_skybox.SetDirectionalLightMarkerAngularRadius(radians); }
        NodeHandle AddNode(const std::shared_ptr<IRenderNode>& renderNode);
        void SetRenderNodePreset(std::shared_ptr<IRenderNode> renderNode);
        void UseDefaultRenderNodePreset();
        PassHandle AddPass(const std::shared_ptr<IRenderPass>& renderPass);
        PassHandle AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        PassHandle AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        bool ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        bool AddPhaseCompletionNode(std::string_view phaseTag,
                                    std::string_view nodeName,
                                    const PhaseCompletionCallback& execute,
                                    PhaseCompletionMode mode = PhaseCompletionMode::Deterministic,
                                    const RenderPassRequirements& requirements = {});
        void ClearPhaseCompletionNodes();
        void ClearPasses();
        const std::vector<RenderPassType>& GetRenderPassSequence() const { return m_passRegistry.GetRenderPassSequence(); }
        void SetRenderPassSequence(const std::vector<RenderPassType>& sequence);
        void RefreshEnvironmentAssets();
        void SetGraphicsRuntime(GraphicsRuntime runtime) { m_graphicsRuntime = runtime; }
        GraphicsRuntime GetGraphicsRuntime() const { return m_graphicsRuntime; }
        void SetRHIBackend(RHIBackend backend) { SetGraphicsRuntime(backend); }
        RHIBackend GetRHIBackend() const { return GetGraphicsRuntime(); }
        void SetGraphicsBackend(GraphicsBackend backend) { SetRHIBackend(backend); }
        GraphicsBackend GetGraphicsBackend() const { return GetRHIBackend(); }

        // Whole-struct access for persistence (save/restore the full render config).
        // m_settings is a RenderFeatureSettings (adds clamping helpers, no extra data),
        // so assigning through the RenderSettings base copies all persisted fields.
        const RenderSettings& GetRenderSettings() const { return m_settings; }
        void SetRenderSettings(const RenderSettings& settings) { static_cast<RenderSettings&>(m_settings) = settings; }

        float GetIblIntensity() const { return m_settings.iblIntensity; }
        void SetIblIntensity(float intensity) { m_settings.SetIblIntensity(intensity); }
        void ResizeViewport(UINT width, UINT height);
        bool GetUseTessellation() const { return m_settings.useTessellation; }
        void SetUseTessellation(bool enable) { m_settings.useTessellation = enable; }
        bool GetTessWireframeEnabled() const { return m_settings.tessWireframeEnabled; }
        void SetTessWireframeEnabled(bool enable) { m_settings.tessWireframeEnabled = enable; }
        bool GetTessDebugColorsEnabled() const { return m_settings.tessDebugColorsEnabled; }
        void SetTessDebugColorsEnabled(bool enable) { m_settings.tessDebugColorsEnabled = enable; }
        bool GetMeshletDebugViewEnabled() const { return m_settings.meshletDebugViewEnabled; }
        void SetMeshletDebugViewEnabled(bool enable) { m_settings.meshletDebugViewEnabled = enable; }
        bool GetUseMeshShader()   const { return m_settings.useMeshShader; }
        void SetUseMeshShader(bool enable) { m_settings.useMeshShader = enable; }
        RenderPathMode GetRenderPathMode() const { return m_settings.renderPathMode; }
        void SetRenderPathMode(RenderPathMode mode);
        RayTracingPerformancePreset GetRayTracingPerformancePreset() const { return m_settings.rayTracingPerformancePreset; }
        void SetRayTracingPerformancePreset(RayTracingPerformancePreset preset) { m_settings.rayTracingPerformancePreset = preset; }
        bool GetRayTracingDynamicResolutionEnabled() const { return m_settings.rayTracingDynamicResolutionEnabled; }
        void SetRayTracingDynamicResolutionEnabled(bool enabled) { m_settings.rayTracingDynamicResolutionEnabled = enabled; }
        uint32_t GetRayTracingMaxBounceCount() const { return m_settings.rayTracingMaxBounceCount; }
        void SetRayTracingMaxBounceCount(uint32_t count) { m_settings.SetRayTracingMaxBounceCount(count); }
        bool GetRasterSoftwareRayTracedDirectionalShadowEnabled() const
        {
            return m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled;
        }
        void SetRasterSoftwareRayTracedDirectionalShadowEnabled(bool enabled)
        {
            const RenderFeatureSettingChanges changes =
                m_settings.SetRasterSoftwareRayTracedDirectionalShadowEnabled(enabled);
            if (changes.invalidateShadowCache) {
                m_swrtExecutor.InvalidateShadowCache();
            }
        }
        bool GetRasterSoftwareRayTracedReflectionEnabled() const
        {
            return m_settings.rasterSoftwareRayTracedReflectionEnabled;
        }
        void SetRasterSoftwareRayTracedReflectionEnabled(bool enabled)
        {
            const RenderFeatureSettingChanges changes =
                m_settings.SetRasterSoftwareRayTracedReflectionEnabled(enabled);
            if (changes.invalidateSceneColorHistory) {
                m_sceneColorHistoryValid = false;
            }
            if (changes.reallocateReflectionResources) {
                m_swrtExecutor.OnReflectionResourcesReallocated();
            }
        }
        bool GetRasterScreenSpaceReflectionEnabled() const
        {
            return m_settings.rasterScreenSpaceReflectionEnabled;
        }
        void SetRasterScreenSpaceReflectionEnabled(bool enabled)
        {
            const RenderFeatureSettingChanges changes =
                m_settings.SetRasterScreenSpaceReflectionEnabled(enabled);
            if (changes.reallocateReflectionResources) {
                m_swrtExecutor.OnReflectionResourcesReallocated();
            }
            if (changes.invalidateSceneColorHistory) {
                m_sceneColorHistoryValid = false;
            }
        }
        float GetSSRMaxDistance() const { return m_settings.ssrMaxDistance; }
        void SetSSRMaxDistance(float v) { m_settings.ssrMaxDistance = v; }
        float GetSSRThickness() const { return m_settings.ssrThickness; }
        void SetSSRThickness(float v) { m_settings.ssrThickness = v; }
        float GetSSRStepCount() const { return m_settings.ssrStepCount; }
        void SetSSRStepCount(float v) { m_settings.ssrStepCount = v; }
        float GetSSRRoughnessCutoff() const { return m_settings.ssrRoughnessCutoff; }
        void SetSSRRoughnessCutoff(float v) { m_settings.ssrRoughnessCutoff = v; }
        float GetSSRRefineSteps() const { return m_settings.ssrRefineSteps; }
        void SetSSRRefineSteps(float v) { m_settings.ssrRefineSteps = v; }
        float GetSSREdgeFade() const { return m_settings.ssrEdgeFade; }
        void SetSSREdgeFade(float v) { m_settings.ssrEdgeFade = v; }
        float GetSSRNormalOffset() const { return m_settings.ssrNormalOffset; }
        void SetSSRNormalOffset(float v) { m_settings.ssrNormalOffset = v; }
        float GetSSRIntensity() const { return m_settings.ssrIntensity; }
        void SetSSRIntensity(float v) { m_settings.ssrIntensity = v; }
        float GetExposure() const { return m_settings.exposure; }
        void SetExposure(float v) { m_settings.exposure = v; }
        bool GetRasterSoftwareRayTracedAmbientOcclusionEnabled() const
        {
            return m_settings.IsRasterSoftwareRayTracedAmbientOcclusionEnabled();
        }
        void SetRasterSoftwareRayTracedAmbientOcclusionEnabled(bool enabled)
        {
            m_settings.SetRasterSoftwareRayTracedAmbientOcclusionEnabled(enabled);
        }
        RendererEnums::AmbientOcclusionMode GetAmbientOcclusionMode() const
        {
            return m_settings.ambientOcclusionMode;
        }
        void SetAmbientOcclusionMode(RendererEnums::AmbientOcclusionMode mode)
        {
            m_settings.SetAmbientOcclusionMode(mode);
        }
        RuntimeAmbientOcclusionMethod GetRuntimeAmbientOcclusionMethod() const { return m_settings.runtimeAoMethod; }
        void SetRuntimeAmbientOcclusionMethod(RuntimeAmbientOcclusionMethod method)
        {
            m_settings.SetRuntimeAmbientOcclusionMethod(method);
        }
        bool GetSwrtUseReSTIR() const { return m_settings.swrtUseReSTIR; }
        void SetSwrtUseReSTIR(bool useReSTIR)
        {
            m_settings.swrtUseReSTIR = useReSTIR;
            ApplySwrtModeSetting();
        }
        uint32_t GetSwrtSamplingMode() const { return m_settings.swrtSamplingMode; }
        void SetSwrtSamplingMode(uint32_t mode) { m_settings.SetSwrtSamplingMode(mode); }
        uint32_t GetSwrtSamplesPerPixel() const { return m_settings.swrtSamplesPerPixel; }
        void SetSwrtSamplesPerPixel(uint32_t n) { m_settings.swrtSamplesPerPixel = n; }
        uint32_t GetSwrtMaxBounces() const { return m_settings.swrtMaxBounces; }
        void SetSwrtMaxBounces(uint32_t n) { m_settings.SetSwrtMaxBounces(n); }
        bool GetSwrtDenoiserEnabled() const { return m_settings.swrtDenoiserEnabled; }
        void SetSwrtDenoiserEnabled(bool enabled) { m_settings.swrtDenoiserEnabled = enabled; }
        uint32_t GetSwrtReflectionAtrousIterations() const { return m_settings.swrtReflectionAtrousIterations; }
        void SetSwrtReflectionAtrousIterations(uint32_t n) { m_settings.SetSwrtReflectionAtrousIterations(n); }
        bool GetGIEnabled()     const { return m_probeGrid.GetEnabled(); }
        void SetGIEnabled(bool e)     { m_probeGrid.SetEnabled(e); }
        float GetGIIntensity()  const { return m_probeGrid.GetGiIntensity(); }
        void SetGIIntensity(float v)  { m_probeGrid.SetGiIntensity(v); }
        float GetGIEmaAlpha()   const { return m_probeGrid.GetEmaAlpha(); }
        void SetGIEmaAlpha(float a)   { m_probeGrid.SetEmaAlpha(a); }
        IrradianceProbeGrid& GetProbeGrid() { return m_probeGrid; }

        // GI bake control (works in all render modes including Raster)
        void  RequestGIBake();
        void  ResetAndRebakeGI();
        void  CancelGIBake();
        bool  IsGIBaking()        const { return m_giBakeRequested; }
        bool  IsGIBaked()         const { return m_probeGrid.IsBaked(); }
        bool  HasGIProbeData()    const { return m_probeGrid.HasEverBaked(); }
        float GetGIBakeProgress() const { return GetGIBakeStatus().progress; }
        GIBakeStatus GetGIBakeStatus() const;
        // Continuous mode: once a full pass over the probe grid completes, immediately
        // restart it instead of stopping, so probes keep tracking scene changes (DDGI-style).
        void  SetGIContinuousMode(bool enabled) { m_giContinuousMode = enabled; }
        bool  GetGIContinuousMode() const { return m_giContinuousMode; }
        bool  SaveGIProbeCache(const std::string& path, uint64_t stateHash);
        bool  LoadGIProbeCache(const std::string& path, uint64_t stateHash);

        // Fits the probe grid to the given world AABB and reallocates the buffer.
        // Safe to call after Initialize()  Euse before first rendered frame.
        void FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                  float bMaxX, float bMaxY, float bMaxZ,
                                  float margin = 1.0f);

        // World AABB over every ray-tracing scene instance. Returns false (outputs
        // untouched) while the scene has no instances yet.
        bool GetSceneWorldBounds(float outMin[3], float outMax[3]) const;

        // Fits the probe grid to the whole loaded scene, coarsening the probe spacing as
        // needed to stay within probeBudget. Returns false and leaves the grid unchanged
        // when the scene bounds are not available yet.
        bool FitProbeGridToSceneAuto(float margin = 2.0f, std::uint32_t probeBudget = 16384u);
        // Volumetric cloud
        bool  GetVolumetricCloudEnabled()  const { return m_settings.volumetricCloudEnabled; }
        void  SetVolumetricCloudEnabled(bool e);
        float GetCloudCover()   const { return m_settings.cloudCover; }
        void  SetCloudCover(float v) { m_settings.cloudCover = v; if (m_volumetricCloudRenderPass) m_volumetricCloudRenderPass->SetCloudCover(v); }
        float GetCloudDensity() const { return m_settings.cloudDensity; }
        void  SetCloudDensity(float v) { m_settings.cloudDensity = v; if (m_volumetricCloudRenderPass) m_volumetricCloudRenderPass->SetCloudDensity(v); }
        float GetCloudWindSpeed() const { return m_settings.cloudWindSpeed; }
        void  SetCloudWindSpeed(float v) { m_settings.cloudWindSpeed = v; if (m_volumetricCloudRenderPass) m_volumetricCloudRenderPass->SetWindSpeed(v); }
        float GetCloudBaseAlt() const { return m_settings.cloudBaseAlt; }
        void  SetCloudBaseAlt(float v) { m_settings.cloudBaseAlt = v; if (m_volumetricCloudRenderPass) m_volumetricCloudRenderPass->SetCloudBaseAlt(v); }
        float GetCloudTopAlt()  const { return m_settings.cloudTopAlt; }
        void  SetCloudTopAlt(float v) { m_settings.cloudTopAlt = v; if (m_volumetricCloudRenderPass) m_volumetricCloudRenderPass->SetCloudTopAlt(v); }

        DebugVisualizationSystem& GetDebugVisualization() { return m_debugVisualization; }
        const DebugVisualizationSystem& GetDebugVisualization() const { return m_debugVisualization; }
        // Re-inserts the debug probe grid node into the current pass list.
        // Call after ClearRenderPasses()+AddRenderPass() sequences (e.g. RayMarchApp).
        void ReinsertDebugProbeGrid();

        // Fluid surface (GPU heightfield water simulation).
        bool  GetFluidSurfaceEnabled()   const { return m_fluidSurfaceRenderPass ? m_fluidSurfaceRenderPass->IsEnabled() : false; }
        void  SetFluidSurfaceEnabled(bool e)   { if (m_fluidSurfaceRenderPass) { m_fluidSurfaceRenderPass->SetEnabled(e); if (e) EnsureFluidSurfacePassInserted(); } }
        void  SetFluidGridOrigin(float x, float z)                 { m_fluidSim.SetGridOrigin(x, z); }
        void  SetFluidCellSize(float cellSize)                     { m_fluidSim.SetCellSize(cellSize); }
        void  SetFluidGridCount(uint32_t countX, uint32_t countZ)  { m_fluidSim.SetGridCount(countX, countZ); }
        void  FitFluidToSceneBounds(float minX, float minZ, float maxX, float maxZ, float margin = 1.0f)
        {
            m_fluidSim.FitToSceneBounds(minX, minZ, maxX, maxZ, margin);
        }
        float GetFluidSurfaceHeight() const { return m_fluidSim.GetSurfaceHeight(); }
        void  SetFluidSurfaceHeight(float y) { m_fluidSim.SetSurfaceHeight(y); }
        float GetFluidWaveSpeed()     const { return m_fluidSim.GetWaveSpeed(); }
        void  SetFluidWaveSpeed(float v)     { m_fluidSim.SetWaveSpeed(v); }
        float GetFluidDamping()       const { return m_fluidSim.GetDamping(); }
        void  SetFluidDamping(float d)       { m_fluidSim.SetDamping(d); }
        bool  GetFluidSimEnabled()    const { return m_fluidSim.GetEnabled(); }
        void  SetFluidSimEnabled(bool e)     { m_fluidSim.SetEnabled(e); }
        void  InjectFluidSplash(float worldX, float worldZ, float radius = 0.5f, float strength = 1.0f)
        {
            m_fluidSim.InjectSplash({ worldX, worldZ, radius, strength });
        }
        // Re-inserts the fluid surface node into the current pass list.
        // Call after ClearRenderPasses()+AddRenderPass() sequences (e.g. RayMarchApp).
        void ReinsertFluidSurface();

        bool  GetParticlesEnabled()      const { return m_particleRenderPass ? m_particleRenderPass->IsEnabled() : false; }
        void  SetParticlesEnabled(bool e)      { if (m_particleRenderPass) { m_particleRenderPass->SetEnabled(e); if (e) EnsureParticlePassInserted(); } }
        float GetParticleEmissionRate()  const { return m_particleSystem.GetEmissionRate(); }
        void  SetParticleEmissionRate(float v) { m_particleSystem.SetEmissionRate(v); }
        float GetParticleGravity()       const { return m_particleSystem.GetGravity(); }
        void  SetParticleGravity(float g)      { m_particleSystem.SetGravity(g); }
        float GetParticleDrag()          const { return m_particleSystem.GetDrag(); }
        void  SetParticleDrag(float d)         { m_particleSystem.SetDrag(d); }
        void  SetParticleEmitOrigin(float x, float y, float z) { m_particleSystem.SetEmitOrigin(x, y, z); }
        // Re-inserts the particle node into the current pass list.
        // Call after ClearRenderPasses()+AddRenderPass() sequences (e.g. RayMarchApp).
        void ReinsertParticles();
        GBufferDebugView GetGBufferDebugView() const { return m_settings.gBufferDebugView; }
        void SetGBufferDebugView(GBufferDebugView view)
        {
            m_settings.SetGBufferDebugView(view);
        }
        void CycleGBufferDebugView(int delta = 1)
        {
            m_settings.CycleGBufferDebugView(delta);
        }
        float GetDeltaTime() const { return m_deltaTime; }
        bool GetRuntimeAOEnabled() const { return m_settings.runtimeAoEnabled; }
        void SetRuntimeAOEnabled(bool enabled) { m_settings.runtimeAoEnabled = enabled; }
        float GetRuntimeAORadius() const { return m_settings.runtimeAoRadius; }
        void SetRuntimeAORadius(float r) { m_settings.SetRuntimeAORadius(r); }
        float GetRuntimeAOBias() const { return m_settings.runtimeAoBias; }
        void SetRuntimeAOBias(float b) { m_settings.runtimeAoBias = b; }
        float GetRuntimeAOIntensity() const { return m_settings.runtimeAoIntensity; }
        void SetRuntimeAOIntensity(float i) { m_settings.SetRuntimeAOIntensity(i); }
        float GetRuntimeAOThickness() const { return m_settings.runtimeAoThickness; }
        void SetRuntimeAOThickness(float t) { m_settings.SetRuntimeAOThickness(t); }
        uint32_t GetRuntimeAOQuality() const { return m_settings.runtimeAoQuality; }
        void SetRuntimeAOQuality(uint32_t q) { m_settings.SetRuntimeAOQuality(q); }
        bool GetSSAOEnabled() const { return GetRuntimeAOEnabled(); }
        void SetSSAOEnabled(bool enabled) { SetRuntimeAOEnabled(enabled); }
        float GetSSAORadius() const { return GetRuntimeAORadius(); }
        void SetSSAORadius(float r) { SetRuntimeAORadius(r); }
        float GetSSAOBias() const { return GetRuntimeAOBias(); }
        void SetSSAOBias(float b) { SetRuntimeAOBias(b); }
        float GetSSAOIntensity() const { return GetRuntimeAOIntensity(); }
        void SetSSAOIntensity(float i) { SetRuntimeAOIntensity(i); }
        float GetSSAOThickness() const { return GetRuntimeAOThickness(); }
        void SetSSAOThickness(float t) { SetRuntimeAOThickness(t); }
        uint32_t GetSSAOQuality() const { return GetRuntimeAOQuality(); }
        void SetSSAOQuality(uint32_t q) { SetRuntimeAOQuality(q); }
        uint32_t GetSwrtAoSampleCount() const { return m_settings.swrtAoSampleCount; }
        void SetSwrtAoSampleCount(uint32_t count)
        {
            m_settings.SetSwrtAoSampleCount(count);
        }
        bool GetVsmBlurEnabled() const { return m_settings.vsmBlurEnabled; }
        void SetVsmBlurEnabled(bool enabled) { m_settings.vsmBlurEnabled = enabled; }
        float GetAoMinOcclusion() const { return m_settings.aoMinOcclusion; }
        void  SetAoMinOcclusion(float v)
        {
            m_settings.SetAoMinOcclusion(v);
        }
        float GetAoDirectLightingStrength() const { return m_settings.aoDirectLightingStrength; }
        void  SetAoDirectLightingStrength(float v)
        {
            m_settings.SetAoDirectLightingStrength(v);
        }
        DirectionalLightSettings GetDirectionalLightSettings() const;
        void SetDirectionalLightSettings(const DirectionalLightSettings& settings);
        std::vector<PointLight>& GetPointLights() { return m_lightSystem.GetPointLights(); }
        const std::vector<PointLight>& GetPointLights() const { return m_lightSystem.GetPointLights(); }
        std::vector<SpotLight>& GetSpotLights() { return m_lightSystem.GetSpotLights(); }
        const std::vector<SpotLight>& GetSpotLights() const { return m_lightSystem.GetSpotLights(); }
        void* GetNativeDeviceHandle() const { return m_device ? m_device->GetNativeDeviceHandle() : nullptr; }
        void* GetNativeGraphicsQueueHandle() const { return m_device ? m_device->GetNativeGraphicsQueueHandle() : nullptr; }
        const RhiBackendCapabilities& GetBackendCapabilities() const;
        bool SupportsFeatureRenderPasses() const { return GetBackendCapabilities().supportsFeatureRenderPasses; }
        bool SupportsD3D12OverlayRendering() const { return GetBackendCapabilities().supportsD3D12CompatibilitySurface; }
        ID3D12Device* GetNativeDevice() const { return m_device ? m_device->GetDevice() : nullptr; }
        ID3D12CommandQueue* GetNativeCommandQueue() const { return m_device ? m_device->GetCommandQueue().Get() : nullptr; }
        bool IsHardwareRayTracingSupported() const;
        const RayTracingStats& GetRayTracingStats() const { return m_rayTracingStats; }
        Format GetBackBufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
        Format GetDepthFormat() const { return DXGI_FORMAT_D32_FLOAT; }
        UINT GetBackBufferCount() const { return 2; }

    private:
        using DrawSceneItemsCallback = std::function<void(bool drawTransparent)>;
        using DrawShadowItemsCallback = std::function<void(const LightSystem::ShadowPassContext&)>;

        struct DeferredUploadBatch
        {
            CommandAllocator allocator;
            CommandList commandList;
            std::vector<Resource> uploadResources;
            UINT64 retireFenceValue = 0;
        };

        void RetireDeferredUploadBatches();
        // Waits (if needed) for the GPU to finish the previous frame's compute work
        // that used the given frame-buffer slot's compute allocator/command list,
        // so it is safe to Reset(). Safe to call on frames that submitted no compute
        // work (the fence value for an unused slot never advances past what the wait
        // already satisfied). Returns false only on a wait failure.
        bool WaitForComputeFrameFence(UINT frameIndex);
        void ApplySwrtModeSetting()
        {
            using SwrtMode = GpuSoftwareRayTracer::SwrtMode;
            m_gpuSoftwareRayTracer.SetMode(m_settings.swrtUseReSTIR ? SwrtMode::ReSTIR : SwrtMode::Legacy);
        }
        RenderPassExecutionPolicy BuildRenderPassExecutionPolicy(bool useShadowTessPath);
        RenderPassFrameInputs BuildRenderPassFrameInputs(CommandList* cmdList,
                                                         IRhiCommandEncoder* commandEncoder,
                                                         RendererFrameCoordinator::FrameContext* frame,
                                                         D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                         GpuDescriptorHandle defaultAoSrv);
        RenderPassExecutionServices BuildRenderPassExecutionServices(const DrawSceneItemsCallback& drawItems,
                                                                     const DrawShadowItemsCallback& drawShadowItems);
        bool HasRenderPass(std::string_view tag) const;
        void EnsureVolumetricCloudPassInserted();
        // Inserts the debug probe-grid pass into the graph after Skybox/Lighting if it is
        // initialized and not already present. Idempotent; safe to call before init (no-op).
        void EnsureDebugProbeGridPassInserted();
        // Inserts the fluid surface pass into the graph after Skybox/Lighting if it is
        // initialized and not already present. Idempotent; safe to call before init (no-op).
        void EnsureFluidSurfacePassInserted();
        void EnsureParticlePassInserted();

        void TransitionBackBufferToRenderTarget(CommandList* cmdList, UINT backIndex);
        void ClearAndBindMainTargets(CommandList* cmdList, UINT backIndex);
        void BindMainTargets(CommandList* cmdList, UINT backIndex);
        void TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex);
        bool CompositeSoftwareReflections(CommandList* cmdList,
                                          UINT backIndex,
                                          D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu);
        bool CopySceneColorForTransmission(CommandList* cmdList);
        bool ToneMapSceneColor(CommandList* cmdList, UINT backIndex);
        void CaptureSceneColorHistory(CommandList* cmdList, UINT backIndex);
        // Bracketed around the frame submission by SubmitAndPresent: the first records the
        // back-buffer copy while the command list is still open, the second waits for the
        // GPU and encodes the PNG once that command list has actually run.
        void RecordPendingScreenshotCopy(CommandList* cmdList, UINT backIndex);
        void ResolvePendingScreenshot();
        void SubmitAndPresent(CommandList* cmdList, UINT frameIndex);
        Texture* CreateTextureFromRgba8Data(const CpuTextureRgba8& src, CommandList* cmdList,
                                            std::vector<Resource>& uploads);
        void RefreshGIBakeStatus(GIBakeState state, uint32_t bvhMissingMask = 0u);
        void SetGIBakePhase(const char* phase);
        void AddGIBakeLog(const char* phase, const char* format, ...);
        std::unique_ptr<IRHIDevice> m_device;
        RendererReadyState m_readyState;
        RenderPipelineStateCache m_pipelineStateCache;
        RendererFrameCoordinator m_frameCoordinator;
        MeshBuffer m_meshBuffer;
        SkinnedMeshBuffer m_skinnedMeshBuffer;
        DrawCommandBuilder m_drawCommandBuilder;
        Skybox m_skybox;
        LightSystem m_lightSystem;
        RenderGraph m_renderGraph;
        RenderPassBuilderCatalog m_renderPassBuilderCatalog;
        std::shared_ptr<ShadowRenderPass> m_shadowRenderPass;
        std::shared_ptr<GBufferRenderPass> m_opaqueGBufferRenderPass;
        std::shared_ptr<DeferredLightingRenderPass> m_lightingRenderPass;
        std::shared_ptr<TransparentLightingRenderPass> m_transparentLightingRenderPass;
        std::shared_ptr<TransparentBackfaceDistanceRenderPass> m_transparentBackfaceDistanceRenderPass;
        std::shared_ptr<TransparentCompositeRenderPass> m_transparentCompositeRenderPass;
        std::shared_ptr<TransparentSceneColorCopyRenderPass> m_transparentSceneColorCopyRenderPass;
        std::shared_ptr<SkyboxRenderPass> m_skyboxRenderPass;
        std::shared_ptr<PostProcessRenderPass> m_postProcessRenderPass;
        std::shared_ptr<RayTracingRenderPass> m_rayTracingRenderPass;
        std::shared_ptr<SSAORenderPass> m_ssaoRenderPass;
        std::shared_ptr<SSAOBlurRenderPass> m_ssaoBlurRenderPass;
        std::shared_ptr<ScreenSpaceReflectionRenderPass> m_screenSpaceReflectionRenderPass;
        std::shared_ptr<ScreenSpaceReflectionCompositeRenderPass> m_screenSpaceReflectionCompositeRenderPass;
        std::shared_ptr<SoftwareReflectionRenderPass> m_softwareReflectionRenderPass;
        std::shared_ptr<SoftwareReflectionCompositeRenderPass> m_softwareReflectionCompositeRenderPass;
        std::shared_ptr<ProceduralSkyRenderPass> m_proceduralSkyRenderPass;
        std::shared_ptr<VolumetricCloudRenderPass> m_volumetricCloudRenderPass;
        DebugVisualizationSystem m_debugVisualization;
        std::shared_ptr<FluidSurfaceRenderPass> m_fluidSurfaceRenderPass;
        FluidHeightfieldSim m_fluidSim;
        std::shared_ptr<ParticleRenderPass> m_particleRenderPass;
        ParticleSystem m_particleSystem;

        SrvDescriptorAllocator m_srvAllocator;
        GpuDescriptorHandle m_nullTextureSrv{};
        Texture* m_defaultAlbedoTexture     = nullptr;
        Texture* m_defaultOcclusionTexture  = nullptr;
        Texture* m_defaultNormalTexture     = nullptr;
        std::vector<std::unique_ptr<Texture>> m_defaultTextures; // owns default albedo/AO fallbacks
        Viewport m_viewport{};
        Rect m_scissorRect{};
        bool m_comInitialized = false;

        CameraState m_cameraState;

        RenderTargetPool m_renderTargetPool;
        
        // Debug screenshot state. m_screenshotRequested means "record a copy this frame";
        // m_screenshotCopyRecorded means "a copy is in the submitted command list and the
        // PNG still has to be written". Both are main-thread only.
        BackBufferCapture m_backBufferCapture;
        std::wstring      m_screenshotPath;
        std::string       m_screenshotResult;
        bool              m_screenshotRequested    = false;
        bool              m_screenshotCopyRecorded = false;
        bool              m_screenshotResultReady  = false;

        // Per-pass GPU timing. The counter is monotonic (the swap-chain back-buffer index
        // is not: it wraps every GetBackBufferCount() frames, which would alias distinct
        // frames onto the same profiler ring slot).
        GpuTimestampProfiler m_gpuTimestampProfiler;
        uint64_t             m_gpuProfilerFrameCounter = 0;

        std::vector<DeferredUploadBatch> m_deferredUploadBatches;
        RayTracingScene m_rayTracingScene;
        GpuSoftwareRayTracer m_gpuSoftwareRayTracer;
        IrradianceProbeGrid m_probeGrid;
        bool     m_giBakeRequested    = false;
        bool     m_giContinuousMode   = false;
        bool     m_giBakeClearPending = false;
        uint32_t m_giBakeFrameIndex   = 0u;
        GIBakeStatus m_giBakeStatus{};
        uint32_t m_giBakeLogSequence = 0u;
        uint32_t m_giBakeLastLoggedMissingMask = 0xFFFFFFFFu;
        uint32_t m_giBakeLastLoggedCompletedProbes = 0xFFFFFFFFu;
        DxrRayTracer m_dxrRayTracer;
        DxrRayTracer::DescriptorSet m_rayTracingDescriptors{};

        SWRTExecutor  m_swrtExecutor;
        SceneSubmitter m_sceneSubmitter;

        RenderFeatureSettings m_settings;
        RayTracingStats m_rayTracingStats{};
        bool m_sceneColorHistoryValid = false;
        float m_deltaTime = 0.0f;
        float m_sceneTime = 0.0f;
        GraphicsRuntime m_graphicsRuntime = GetBuildDefaultGraphicsRuntime();
        RenderPassRegistry m_passRegistry;

        // Async compute resources (created in Initialize; null if compute queue unavailable)
        std::vector<CommandAllocator> m_computeAllocators; // per frame-buffer
        CommandList                   m_computeCmdList;
        bool                          m_computeCmdListReady = false;
        ComPtr<ID3D12Fence>           m_crossQueueFence;
        UINT64                        m_crossQueueFenceVal  = 0;

        // Per frame-buffer slot: fence value the compute queue was signaled with
        // after that slot's compute command list was last submitted (0 if that
        // slot's allocator/command list have never been used). Reuses
        // m_crossQueueFence's timeline. Waited on by WaitForComputeFrameFence()
        // before Reset()-ing the slot on a later frame, so the reset is safe even
        // though RendererFrameCoordinator's graphics-only frame fence never waits
        // on compute queue work.
        std::vector<UINT64> m_computeFrameFenceValues;
        HANDLE               m_computeFrameFenceEvent = nullptr;

        // Extra graphics command list slots opened when a Node ends at a genuine
        // cross-queue join (RenderGraph::Execute's submitCurrentGraphicsNode /
        // acquireNextGraphicsNode). One vector per frame-buffer slot; grows on
        // demand as additional joins occur within a single frame. Reuse across
        // frames is safe under the same guarantee as the primary per-frame command
        // list: RendererFrameCoordinator::BeginFrame waits for that frame-buffer
        // slot's frame fence (signaled only after all of that frame's graphics
        // work, including every Node command list, has been submitted) before any
        // of the slot's allocators are reset again.
        struct GraphicsNodeCommandList
        {
            CommandAllocator allocator;
            CommandList      cmdList;
        };
        std::vector<std::vector<GraphicsNodeCommandList>> m_graphicsNodeCommandLists; // [frame-buffer][node slot]

        // Composed subsystems  Emust be declared after all members they reference.
        SceneSynchronizer  m_sceneSynchronizer;
        EnvironmentManager m_environmentManager;
    };
}
