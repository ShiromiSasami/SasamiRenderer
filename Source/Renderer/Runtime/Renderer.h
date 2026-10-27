#pragma once
#include "Renderer/Scene/SceneSynchronizer.h"
#include "Renderer/Scene/EnvironmentManager.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Config/BackendFeatureSupport.h"
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
#include "Renderer/Passes/PostProcess/BloomRenderer.h"
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
#include "Renderer/Passes/Geometry/MeshShaderRenderPass.h"
#include "Renderer/VirtualGeometry/ClusterDrawSubmitter.h"
#include "Renderer/VirtualGeometry/ClusterFrameUpdate.h"
#include "Renderer/VirtualGeometry/ClusterGpuCulling.h"
#include "Renderer/VirtualGeometry/ClusterInstanceBuffer.h"
#include "Renderer/VirtualGeometry/ClusterMaterialTable.h"
#include "Renderer/VirtualGeometry/ClusterResidencyManager.h"
#include "Renderer/VirtualGeometry/ClusterPagePool.h"
#include "Renderer/VirtualGeometry/ClusterStreamingFeedback.h"
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
#include "Renderer/Passes/Fluid/FluidBackdropCopyRenderPass.h"
#include "Renderer/Passes/Fluid/SmokeVolumeRenderPass.h"
#include "Renderer/Passes/Fluid/ParticleVolumeRenderPass.h"
#include "Renderer/Fluid/SmokeVolumeSim.h"
#include "Renderer/Fluid/FluidHeightfieldSim.h"
#include "Renderer/Passes/Particles/ParticleRenderPass.h"
#include "Renderer/Particles/ParticleSystem.h"
#include "Renderer/Passes/Geometry/CrowdRenderPass.h"
#include "Renderer/Passes/Geometry/WireframeOverlayRenderPass.h"
#include "Renderer/Crowd/CrowdSystem.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/RayTracing/DxrRayTracer.h"
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include "Renderer/GI/IrradianceProbeGrid.h"
#include "Renderer/Scene/Skybox.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/Runtime/RendererReadyState.h"
#include "Renderer/Runtime/FrameRenderContext.h"
#include "Renderer/Capture/TextureCapture.h"
#include "Renderer/Profiling/GpuTimestampProfiler.h"
#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Foundation layer types: declared in their own namespace so these stay the same entities as
// the definitions, not new SasamiEngine:: types that happen to share a name.
namespace SasamiEngine::Foundation { class InitTaskScheduler; }

namespace SasamiEngine::Render
{

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

        using RenderProxy        = SasamiEngine::Render::RenderProxy;
        using SkinnedRenderProxy = SasamiEngine::Render::SkinnedRenderProxy;

        using OverlayRenderCallback = std::function<void(CommandList&, CpuDescriptorHandle)>;
        // Records extra compute work into the frame's graphics encoder after the built-in simulations
        // (DX12 frame path only; not called on native backends or when the graph did not execute).
        using FrameComputeHook = std::function<void(IRhiCommandEncoder&)>;
        using PhaseCompletionMode = RenderGraph::PhaseCompletionMode;
        using PhaseCompletionCallback = std::function<bool(const RenderPassContextView&)>;

        using RayTracingStats = SasamiEngine::Render::RayTracingStats;

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

        // Scene-level geometry ownership (meshes, cluster DAGs). Exposed for the
        // debug command layer; render passes receive it through the frame inputs.
        SceneSubmitter&       GetSceneSubmitter()       { return m_sceneSubmitter; }
        const SceneSubmitter& GetSceneSubmitter() const { return m_sceneSubmitter; }

        // Overwrites the uploaded vertex buffer of submitted mesh `meshIndex` (SceneSubmitter::GetMeshes()
        // order) in place, for a mesh whose topology stays fixed while its vertices animate. Fails when
        // the mesh has no RHI vertex buffer or `count` differs from its vertex count. Debt: the buffer is
        // CpuToGpu and written while earlier frames may still read it (no ring); the CPU copy in the
        // submitter, bounds, ray-tracing BLAS and cluster DAG keep the old vertices.
        bool UpdateStaticMeshVertices(size_t meshIndex, const Vertex* vertices, size_t count);
        // Writes vertices [firstVertex, firstVertex + count) of mesh `meshIndex` into the next copy of a vertex
        // buffer ring (MeshBuffer::WriteDynamicVertices, GetBackBufferCount() + 1 copies, created on the first
        // call), so no copy is rewritten while a frame in flight reads it. `vertices` is the whole array and
        // vertexCount must equal the uploaded count: the ranges the other copies missed are copied from it too.
        // Several calls between two Render() calls write the same copy. Do not mix with UpdateStaticMeshVertices
        // on the same mesh. The submitter's CPU copy, bounds, ray-tracing BLAS and cluster DAG keep the uploaded
        // vertices. The copy count assumes the DX12 frame coordinator (one fence per back buffer); the other
        // backends' frame latency was not checked.
        bool UpdateStaticMeshDynamicVertices(size_t meshIndex, const Vertex* vertices, size_t vertexCount,
                                             size_t firstVertex, size_t count, size_t* writtenVertices = nullptr);
        // Vertex-path draws of mesh `meshIndex` use only its first `indexCount` indices until the next upload
        // (MeshBuffer::SetDrawIndexCount). For fixed-capacity dynamic meshes, so unused slots cost nothing.
        bool SetStaticMeshDrawIndexCount(size_t meshIndex, size_t indexCount);

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
        void MarkRasterSubsystemsReady();
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
        // name: "backbuffer" / "shadow0".."shadow3" / "gbuffer.albedo" / "gbuffer.normal" / "gbuffer.material" / "gbuffer.emissive" / "gbuffer.specular"
        // Return value is empty string on success, non-empty error message otherwise.
        std::string RequestTargetDump(const std::string& name, const std::wstring& path);
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
        void UseToonRenderNodePreset();
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
        // Bloom (BloomRenderer, run inside ToneMapSceneColor). Off by default -- see RenderSettings.h.
        bool GetBloomEnabled() const { return m_settings.bloomEnabled; }
        void SetBloomEnabled(bool enable) { m_settings.bloomEnabled = enable; }
        float GetBloomIntensity() const { return m_settings.bloomIntensity; }
        void SetBloomIntensity(float intensity) { m_settings.bloomIntensity = intensity; }
        float GetBloomRadius() const { return m_settings.bloomRadius; }
        void SetBloomRadius(float radius) { m_settings.bloomRadius = radius; }
        // God rays (LightShaftRenderPass). Off by default -- see RenderSettings.h.
        bool GetLightShaftEnabled() const { return m_settings.lightShaftEnabled; }
        void SetLightShaftEnabled(bool enable) { m_settings.lightShaftEnabled = enable; }
        float GetLightShaftIntensity() const { return m_settings.lightShaftIntensity; }
        void SetLightShaftIntensity(float intensity) { m_settings.lightShaftIntensity = intensity; }

        // Lens flare (LensFlareRenderPass). Off by default -- see RenderSettings.h.
        bool GetLensFlareEnabled() const { return m_settings.lensFlareEnabled; }
        void SetLensFlareEnabled(bool enable) { m_settings.lensFlareEnabled = enable; }
        float GetLensFlareIntensity() const { return m_settings.lensFlareIntensity; }
        void SetLensFlareIntensity(float intensity) { m_settings.lensFlareIntensity = intensity; }
        int GetLensFlareBladeCount() const { return m_settings.lensFlareBladeCount; }
        // 0 = authored sprite table, 1 = Silicon Studio procedural, 2 = ray traced (not yet).
        int GetLensFlareMode() const { return m_settings.lensFlareMode; }
        void SetLensFlareMode(int mode) { m_settings.lensFlareMode = mode; }
        void SetLensFlareBladeCount(int blades) { m_settings.lensFlareBladeCount = blades; }

        bool GetUseTessellation() const { return m_settings.useTessellation; }
        void SetUseTessellation(bool enable) { m_settings.useTessellation = enable; }
        bool GetTessWireframeEnabled() const { return m_settings.tessWireframeEnabled; }
        void SetTessWireframeEnabled(bool enable) { m_settings.tessWireframeEnabled = enable; }
        bool GetTessDebugColorsEnabled() const { return m_settings.tessDebugColorsEnabled; }
        void SetTessDebugColorsEnabled(bool enable) { m_settings.tessDebugColorsEnabled = enable; }
        bool GetUseMeshShader()   const { return m_settings.useMeshShader; }
        void SetUseMeshShader(bool enable) { m_settings.useMeshShader = enable; }
        float GetClusterLodThresholdPx() const { return m_settings.clusterLodThresholdPx; }
        void SetClusterLodThresholdPx(float pixels) { m_settings.clusterLodThresholdPx = pixels; }
        // In shadow-map texels, not screen pixels: shadow views run the ortho LOD mode.
        float GetClusterShadowLodThresholdPx() const { return m_settings.clusterShadowLodThresholdPx; }
        void SetClusterShadowLodThresholdPx(float texels) { m_settings.clusterShadowLodThresholdPx = texels; }
        bool GetClusterShadowSelectOnly() const { return m_settings.clusterShadowSelectOnly; }
        void SetClusterShadowSelectOnly(bool selectOnly) { m_settings.clusterShadowSelectOnly = selectOnly; }
        bool GetClusterSelectStats() const { return m_settings.clusterSelectStats; }
        void SetClusterSelectStats(bool enabled) { m_settings.clusterSelectStats = enabled; }
        uint32_t GetClusterDebugMode() const { return m_settings.clusterDebugMode; }
        void SetClusterDebugMode(uint32_t mode) { m_settings.clusterDebugMode = mode; }
        const ClusterDrawStats& GetLastClusterDrawStats() const { return m_lastClusterDrawStats; }
        const ClusterDrawStats& GetLastClusterShadowStats() const { return m_lastClusterShadowStats; }
        const ClusterStreamingFeedback& GetClusterStreamingFeedback() const { return m_clusterStreamingFeedback; }
        const ClusterResidencyManager& GetClusterResidency() const { return m_clusterResidency; }
        const ClusterPagePool& GetClusterPagePool() const { return m_clusterPagePool; }
        ClusterPagePool& GetClusterPagePool() { return m_clusterPagePool; }   // debug commands reset the peak-load record
        bool GetNativeShadowEnabled() const { return m_settings.nativeShadowEnabled; }
        void SetNativeShadowEnabled(bool enable) { m_settings.nativeShadowEnabled = enable; }
        bool GetClusterGpuCulling() const { return m_settings.clusterGpuCulling; }
        void SetClusterGpuCulling(bool enable) { m_settings.clusterGpuCulling = enable; }
        bool GetClusterGpuMainView() const { return m_settings.clusterGpuMainView; }
        void SetClusterGpuMainView(bool enable) { m_settings.clusterGpuMainView = enable; }
        uint32_t GetClusterStreamRetentionFrames() const { return m_clusterResidency.GetStats().retentionFrames; }
        void SetClusterStreamRetentionFrames(uint32_t frames) { m_clusterResidency.SetRetentionFrames(frames); }
        uint32_t GetClusterStreamBudgetGroups() const { return m_settings.clusterStreamBudgetGroups; }
        void SetClusterStreamBudgetGroups(uint32_t budget)
        {
            m_settings.clusterStreamBudgetGroups = budget;
            m_clusterResidency.SetBudget(budget);
        }
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
        // One-of-N view over the reflection flags. Implemented on top of the existing setters
        // rather than replacing the flags, because those setters also carry the bookkeeping
        // (reflection resource reallocation, SceneColor history invalidation) that the passes
        // depend on. The flags keep the invariant "at most one is true"; the setters below
        // enforce it and nothing else writes them.
        // Cel shading controls, forwarded to LightSystem which owns the light constant buffer.
        LightSystem::ToonSettings GetToonSettings() const { return m_lightSystem.GetToonSettings(); }
        void SetToonSettings(const LightSystem::ToonSettings& settings) { m_lightSystem.SetToonSettings(settings); }
        const LightSystem::ToonFaceSettings& GetToonFaceSettings() const { return m_lightSystem.GetToonFaceSettings(); }
        void SetToonFaceSettings(const LightSystem::ToonFaceSettings& settings) { m_lightSystem.SetToonFaceSettings(settings); }
        const LightSystem::ToonHairSettings& GetToonHairSettings() const { return m_lightSystem.GetToonHairSettings(); }
        void SetToonHairSettings(const LightSystem::ToonHairSettings& settings) { m_lightSystem.SetToonHairSettings(settings); }
        const LightSystem::ToonHairShadowSettings& GetToonHairShadowSettings() const { return m_lightSystem.GetToonHairShadowSettings(); }
        void SetToonHairShadowSettings(const LightSystem::ToonHairShadowSettings& settings) { m_lightSystem.SetToonHairShadowSettings(settings); }
        const LightSystem::ToonHeadNormalSettings& GetToonHeadNormalSettings() const { return m_lightSystem.GetToonHeadNormalSettings(); }
        void SetToonHeadNormalSettings(const LightSystem::ToonHeadNormalSettings& settings) { m_lightSystem.SetToonHeadNormalSettings(settings); }
        // Uploads the SDF face-shadow threshold map (R = threshold, UNORM linear) bound at t19 by
        // the GBuffer pass (RendererToonFace.cpp). One map for the whole scene. A replaced map's
        // texture is kept alive in m_defaultTextures (a small leak per call, debug-only use).
        bool SetToonFaceShadowMap(const CpuTextureRgba8& map);
        bool HasToonFaceShadowMap() const { return m_toonFaceShadowMap != nullptr; }

        RendererEnums::IndirectSpecularSource GetIndirectSpecularSource() const
        {
            if (m_settings.rasterSoftwareRayTracedReflectionEnabled) {
                return RendererEnums::IndirectSpecularSource::SoftwareRayTracing;
            }
            if (m_settings.rasterScreenSpaceReflectionEnabled) {
                return RendererEnums::IndirectSpecularSource::ScreenSpace;
            }
            return RendererEnums::IndirectSpecularSource::None;
        }

        void SetIndirectSpecularSource(RendererEnums::IndirectSpecularSource source)
        {
            using Source = RendererEnums::IndirectSpecularSource;
            // Order matters: clear first, then enable, so the two never overlap even for an
            // instant and the setters' "turn the other one off" paths do not fight each other.
            SetRasterSoftwareRayTracedReflectionEnabled(false);
            SetRasterScreenSpaceReflectionEnabled(false);
            if (source == Source::SoftwareRayTracing) {
                SetRasterSoftwareRayTracedReflectionEnabled(true);
            } else if (source == Source::ScreenSpace) {
                SetRasterScreenSpaceReflectionEnabled(true);
            }
            // HardwareRayTracing is deliberately not handled here: it is a whole-pipeline mode
            // today (SetRenderPathMode), not a specular contributor. IsIndirectSpecularSourceAvailable
            // reports it as unavailable so callers refuse it instead of silently doing nothing.
        }

        RendererEnums::IndirectDiffuseSource GetIndirectDiffuseSource() const
        {
            return m_probeGrid.GetEnabled() ? RendererEnums::IndirectDiffuseSource::ProbeGI
                                            : RendererEnums::IndirectDiffuseSource::None;
        }

        void SetIndirectDiffuseSource(RendererEnums::IndirectDiffuseSource source)
        {
            m_probeGrid.SetEnabled(source == RendererEnums::IndirectDiffuseSource::ProbeGI);
        }

        // What the UI greys out and what the debug command refuses, with the reason.
        static bool IsIndirectSpecularSourceAvailable(RendererEnums::IndirectSpecularSource source)
        {
            return source != RendererEnums::IndirectSpecularSource::HardwareRayTracing;
        }

        static bool IsIndirectDiffuseSourceAvailable(RendererEnums::IndirectDiffuseSource source)
        {
            using Source = RendererEnums::IndirectDiffuseSource;
            // SoftwareRayTracing as a *per-pixel* diffuse source means ReSTIR GI, which is not
            // implemented (the current SwrtMode::ReSTIR is direct lighting). Note the probe grid
            // is already traced by SWRT, so "GI" is not a non-ray-traced alternative to it.
            return source == Source::None || source == Source::ProbeGI;
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
        // HDR10 output toggle (DX12 raster path only). The swap chain format / colour space only
        // actually change on the next ResizeViewport call (see Renderer.cpp), not the instant this
        // is set -- see the comment there for why.
        bool GetEnableHdrOutput() const { return m_settings.enableHdrOutput; }
        void SetEnableHdrOutput(bool enable) { m_settings.enableHdrOutput = enable; }
        float GetHdrPaperWhiteNits() const { return m_settings.hdrPaperWhiteNits; }
        void SetHdrPaperWhiteNits(float nits) { m_settings.hdrPaperWhiteNits = nits; }
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

        // Sets the probe spacing and rebuilds the grid, then queues a rebake.
        // FitToSceneBoundsWithBudget only ever coarsens, so raising probeBudget alone can
        // never make the grid finer -- the base spacing has to be set first, which is what
        // this does. probeBudget therefore acts purely as a memory cap here.
        // Returns the spacing actually in use, which is coarser than requested when the
        // budget clamped it. Probe count scales with 1/spacing^3: halving the spacing is
        // eight times the probes, and eight times the bake.
        float SetProbeGridSpacing(float targetSpacing, std::uint32_t probeBudget = 16384u);
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
        // --- Ray-marched particle volume (wind fire/smoke splatted on the CPU, ParticleVolumeRenderPass) ---
        bool GetParticleVolumeEnabled() const { return m_particleVolumeRenderPass ? m_particleVolumeRenderPass->IsEnabled() : false; }
        void SetParticleVolumeEnabled(bool e)
        {
            if (m_particleVolumeRenderPass) {
                m_particleVolumeRenderPass->SetEnabled(e);
                if (e) EnsureParticleVolumePassInserted();
            }
        }
        // cells: dims[0]*dims[1]*dims[2] float4 (smoke density, soot, flame density, flame age), x fastest.
        bool SetParticleVolumeData(const float origin[3], float cellSize, const uint32_t dims[3], const float* cells,
                                   uint64_t version)
        {
            return m_particleVolumeRenderPass ? m_particleVolumeRenderPass->SetVolumeData(origin, cellSize, dims, cells, version)
                                              : false;
        }
        void SetParticleVolumeStyle(const ParticleVolumeStyle& style)
        {
            if (m_particleVolumeRenderPass) m_particleVolumeRenderPass->SetStyle(style);
        }
        ParticleVolumeStyle GetParticleVolumeStyle() const
        {
            return m_particleVolumeRenderPass ? m_particleVolumeRenderPass->Style() : ParticleVolumeStyle{};
        }

        // --- Volumetric smoke (SmokeVolumeSim + SmokeVolumeRenderPass) ---
        // Off by default: the solver dispatches several compute passes per frame over the whole
        // grid whether or not anything is visible, so it is opt-in like the water surface.
        bool  GetSmokeEnabled() const { return m_smokeVolumeRenderPass ? m_smokeVolumeRenderPass->IsEnabled() : false; }
        void  SetSmokeEnabled(bool e)
        {
            m_smokeSim.SetEnabled(e);
            if (m_smokeVolumeRenderPass) {
                m_smokeVolumeRenderPass->SetEnabled(e);
                if (e) EnsureSmokeVolumePassInserted();
            }
        }
        void  SetSmokeEmitter(float x, float y, float z, float radius) { m_smokeSim.SetEmitter(x, y, z, radius); }
        void  SetSmokeEmitterActive(bool active) { m_smokeSim.SetEmitterActive(active); }
        bool  GetSmokeEmitterActive() const { return m_smokeSim.IsEmitterActive(); }
        void  SetSmokeBuoyancy(float b) { m_smokeSim.SetBuoyancy(b); }
        void  SetSmokeBuoyancyMode(SmokeVolumeSim::BuoyancyMode m) { m_smokeSim.SetBuoyancyMode(m); }
        SmokeVolumeSim::BuoyancyMode GetSmokeBuoyancyMode() const { return m_smokeSim.GetBuoyancyMode(); }
        void  SetSmokeTemperatureParams(float beta, float alpha, float emitterT, float cooling) { m_smokeSim.SetTemperatureParams(beta, alpha, emitterT, cooling); }
        const SmokeVolumeSim& GetSmokeSim() const { return m_smokeSim; }
        SmokeVolumeSim& GetSmokeSimMutable() { return m_smokeSim; } // debug readback (smoke.mesh)
        void  SetSmokeDissipation(float d) { m_smokeSim.SetDissipation(d); }
        void  SetSmokeVorticity(float v) { m_smokeSim.SetVorticity(v); }
        void  SetSmokeWind(float x, float y, float z, float strength) { m_smokeSim.SetWind(x, y, z, strength); }
        float GetSmokeWindStrength() const { return m_smokeSim.GetWindStrength(); }
        float GetSmokeVorticity() const { return m_smokeSim.GetVorticity(); }
        void  SetSmokeJacobiIterations(uint32_t n) { m_smokeSim.SetJacobiIterations(n); }
        uint32_t GetSmokeJacobiIterations() const { return m_smokeSim.GetJacobiIterations(); }
        void  SetSmokeGridSize(uint32_t x, uint32_t y, uint32_t z) { m_smokeSim.SetGridSize(x, y, z); }
        void  SetSmokeVolumeOrigin(float x, float y, float z) { m_smokeSim.SetOrigin(x, y, z); }
        void  SetSmokeCellSize(float s) { m_smokeSim.SetCellSize(s); }
        // Solid obstacle mask for the smoke grid; see SmokeVolumeSim::SetSolidMask.
        bool  SetSmokeSolidMask(const uint32_t* words, size_t count, bool clearField = true) { return m_smokeSim.SetSolidMask(words, count, clearField); }
        void  ClearSmokeSolidMask() { m_smokeSim.ClearSolidMask(); }
        bool  IsSmokeSolidActive() const { return m_smokeSim.IsSolidActive(); }
        uint32_t GetSmokeCountX() const { return m_smokeSim.GetCountX(); }
        uint32_t GetSmokeCountY() const { return m_smokeSim.GetCountY(); }
        uint32_t GetSmokeCountZ() const { return m_smokeSim.GetCountZ(); }
        float GetSmokeOriginX() const { return m_smokeSim.GetOriginX(); }
        float GetSmokeOriginY() const { return m_smokeSim.GetOriginY(); }
        float GetSmokeOriginZ() const { return m_smokeSim.GetOriginZ(); }
        float GetSmokeCellSize() const { return m_smokeSim.GetCellSize(); }

        float GetFluidRefractionStrength() const { return m_fluidSurfaceRenderPass ? m_fluidSurfaceRenderPass->GetRefractionStrength() : 0.0f; }
        void  SetFluidRefractionStrength(float s) { if (m_fluidSurfaceRenderPass) { m_fluidSurfaceRenderPass->SetRefractionStrength(s); } }
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

        bool GetCrowdEnabled() const { return m_crowdRenderPass ? m_crowdRenderPass->IsEnabled() : false; }
        void SetCrowdEnabled(bool e) { if (m_crowdRenderPass) { m_crowdRenderPass->SetEnabled(e); if (e) EnsureCrowdPassInserted(); } }
        Crowd::CrowdSystem& GetCrowdSystem() { return m_crowdSystem; }
        CrowdRenderPass* GetCrowdRenderPass() { return m_crowdRenderPass.get(); }
        // Editor wireframe view: the pass is only inserted into the graph once enabled.
        bool GetWireframeOverlayEnabled() const { return m_wireframeOverlayRenderPass ? m_wireframeOverlayRenderPass->IsEnabled() : false; }
        void SetWireframeOverlayEnabled(bool e) { if (m_wireframeOverlayRenderPass) { m_wireframeOverlayRenderPass->SetEnabled(e); if (e) EnsureWireframeOverlayPassInserted(); } }
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
        // runtimeAoEnabled is the master switch for every runtime (non-material) AO path, on all
        // four backends: false collapses the mode to MaterialOnly rather than being OR-ed with it.
        // The D3D12 paths already folded the flag in this direction (RendererDrawCallbacks.cpp,
        // RendererPassServices.cpp); the native backends ORed the two instead, so `render.runtimeao 0`
        // left SSAO running whenever aoMode was anything but MaterialOnly and the A/B was blind.
        // Single-sourcing the expression here is what keeps the two families from drifting again.
        RendererEnums::AmbientOcclusionMode GetEffectiveAmbientOcclusionMode() const
        {
            return m_settings.runtimeAoEnabled
                ? m_settings.ambientOcclusionMode
                : RendererEnums::AmbientOcclusionMode::MaterialOnly;
        }
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
        PunctualLightFalloff GetPunctualLightFalloff() const { return m_settings.punctualLightFalloff; }
        void SetPunctualLightFalloff(PunctualLightFalloff v)
        {
            m_settings.SetPunctualLightFalloff(v);
            m_lightSystem.SetPunctualLightFalloff(m_settings.punctualLightFalloff);
        }
        DirectionalLightSettings GetDirectionalLightSettings() const;
        void SetDirectionalLightSettings(const DirectionalLightSettings& settings);
        float GetShadowDistance() const { return m_lightSystem.GetShadowDistance(); }
        void SetShadowDistance(float v) { m_lightSystem.SetShadowDistance(v); }
        // Debug/measurement: shadow passes still run and clear their maps, but draw no casters.
        bool GetSkipShadowCasters() const { return m_skipShadowCasters; }
        void SetSkipShadowCasters(bool v) { m_skipShadowCasters = v; }
        std::vector<PointLight>& GetPointLights() { return m_lightSystem.GetPointLights(); }
        const std::vector<PointLight>& GetPointLights() const { return m_lightSystem.GetPointLights(); }
        std::vector<SpotLight>& GetSpotLights() { return m_lightSystem.GetSpotLights(); }
        const std::vector<SpotLight>& GetSpotLights() const { return m_lightSystem.GetSpotLights(); }
        void* GetNativeDeviceHandle() const { return m_device ? m_device->GetNativeDeviceHandle() : nullptr; }
        void* GetNativeGraphicsQueueHandle() const { return m_device ? m_device->GetNativeGraphicsQueueHandle() : nullptr; }
        const RhiBackendCapabilities& GetBackendCapabilities() const;
        // What the running backend and GPU can actually do, resolved once after the device
        // exists. The UI reads this to grey out controls whose feature cannot run here.
        const BackendFeatureSupport& GetFeatureSupport() const { return m_featureSupport; }
        bool SupportsFeatureRenderPasses() const { return GetBackendCapabilities().supportsFeatureRenderPasses; }
        bool SupportsD3D12OverlayRendering() const { return GetBackendCapabilities().supportsD3D12CompatibilitySurface; }
        // Whether a UI overlay can be drawn at all this run, on either path: the D3D12 one where
        // the app hands us a command list, or the native one where the backend calls the overlay
        // back from inside its own frame.
        bool SupportsUIOverlay() const;
        // The overlay the native backends call back into. Ignored on the D3D12 path, which keeps
        // taking its OverlayRenderCallback per frame.
        void SetFrameOverlay(IRhiFrameOverlay* overlay) { m_frameOverlay = overlay; }
        void SetFrameComputeHook(FrameComputeHook hook) { m_frameComputeHook = std::move(hook); }
        // What an ImGui renderer backend needs to attach to the running device. False when the
        // backend has none (the D3D12 path answers false; it is driven by the callback instead).
        bool GetOverlayDeviceContext(RhiOverlayDeviceContext& context) const;
        ID3D12Device* GetNativeDevice() const { return m_device ? m_device->GetDevice() : nullptr; }
        // RHI view of the device for code that only needs IRhiDevice (e.g. Voxel::GpuSurfaceVoxelizer).
        IRHIDevice* GetRhiDevice() const { return m_device.get(); }
        ID3D12CommandQueue* GetNativeCommandQueue() const { return m_device ? m_device->GetCommandQueue().Get() : nullptr; }
        bool IsHardwareRayTracingSupported() const;
        const RayTracingStats& GetRayTracingStats() const { return m_rayTracingStats; }
        Format GetBackBufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
        Format GetDepthFormat() const { return DXGI_FORMAT_D32_FLOAT; }
        UINT GetBackBufferCount() const { return 2; }

    private:
        // dynamicVerticesOnly: draw only DrawItem::dynamicVertices items (the ones the cluster path skips).
        using DrawSceneItemsCallback = std::function<void(bool drawTransparent, bool dynamicVerticesOnly)>;
        using DrawShadowItemsCallback = std::function<void(const LightSystem::ShadowPassContext&)>;

        struct DeferredUploadBatch
        {
            CommandAllocator allocator;
            CommandList commandList;
            std::vector<Resource> uploadResources;
            UINT64 retireFenceValue = 0;
        };

        void RetireDeferredUploadBatches();
        // Non-DX12 backends draw their whole frame through ExecuteBackendFrame and never touch
        // the render-pass graph that the rest of Render() builds, so this path is a distinct
        // responsibility from Render() and lives in its own file, RendererNativeFrame.cpp.
        // Returns true when it fully handled the frame (Render() should return immediately)
        // and false when Render() should fall through to the render-pass graph path.
        bool RenderNativeBackendFrame(const OverlayRenderCallback& overlay);
        // Waits (if needed) for the GPU to finish the previous frame's compute work
        // that used the given frame-buffer slot's compute allocator/command list,
        // so it is safe to Reset(). Safe to call on frames that submitted no compute
        // work (the fence value for an unused slot never advances past what the wait
        // already satisfied). Returns false only on a wait failure.
        bool WaitForComputeFrameFence(UINT frameIndex);
        // The other half of that same cross-queue fence protocol: closes this frame's compute
        // command list, executes it on the compute queue and signals the fence value that a
        // later frame's WaitForComputeFrameFence() waits on before reusing this slot. The wait
        // and the signal are one responsibility and share the same members, so both live in
        // RendererComputeQueue.cpp rather than being split across the frame code that calls them.
        void SubmitFrameComputeCommands(CommandList* computeCmdList, UINT computeSlot);
        void ApplySwrtModeSetting()
        {
            using SwrtMode = GpuSoftwareRayTracer::SwrtMode;
            m_gpuSoftwareRayTracer.SetMode(m_settings.swrtUseReSTIR ? SwrtMode::ReSTIR : SwrtMode::Legacy);
        }
        // Render()'s frame-graph construction was originally one ~1400-line function body.
        // These two stages build closures (draw callbacks, render-pass services, the
        // cross-queue graphics-node rotation callbacks) that the render-pass graph keeps
        // invoking well after the stage that built them returns, so each is a distinct
        // responsibility from the rest of Render() and lives in its own file -- see
        // FrameRenderContext.h for how the two share frame-local state with each other and
        // with Render() itself without those closures capturing dangling locals.
        // Stage 4 (RendererDrawCallbacks.cpp): draw-callback construction and the
        // per-frame software ray-traced shadow/reflection/AO feature-gate resolution.
        void PrepareFrameDrawCallbacks(FrameRenderContext& context);
        // Binds the albedo SRV (root 0) read by ShadowAlphaMask_PS, falling back like the GBuffer callbacks.
        void BindShadowAlphaMaskAlbedo(D3D12CommandListRhiEncoder& enc, Texture* texture);
        // Stage 5 (RendererPassServices.cpp): render-pass input/service construction and
        // the render-graph execute context Render() hands to RegisterPassesToRenderGraph.
        void BuildFramePassInputsAndServices(FrameRenderContext& context);
        // The once-per-frame GPU-driven cluster preparation (RendererClusterFrame.cpp), which
        // is not a stage of the pass graph at all: it is a property of the frame, so it runs
        // before the graph (Begin) and closes after it (End) instead of inside a pass. Begin
        // records the page-pool copies on cmdList, so it has to precede every view that reads
        // the pool -- the directional-shadow cascades first.
        void BeginClusterFrameUpdate(CommandList* cmdList, UINT backIndex, bool clusterPathActive);
        void EndClusterFrameUpdate();
        // Sizes this frame's per-draw camera constant buffer (RendererCameraBudget.cpp).
        // Reads the directional/point shadow configuration and the scene's static and skinned
        // draw-item counts to work out how many camera-constant slots this frame's recording
        // will push, then grows the frame's buffer to fit. It is a budgeting decision made from
        // settings and scene counts alone: it records no commands and touches neither the
        // render graph nor any pass, which is why it is not part of the frame-graph code.
        void EnsureFrameCameraConstantBuffers(RendererFrameCoordinator::FrameContext& frame);
        // Ensures this frame's render targets exist at the current viewport size and declares
        // them -- scene color, the transmission and OIT targets, the optional SSR targets, the
        // GBuffer and SSAO targets, plus the back buffer and scene depth -- to the render graph
        // as external resources (RendererFrameTargets.cpp). Like the camera budget above this
        // only reserves and publishes resources; the commands that use them are recorded later.
        // Returns false when a target the frame cannot proceed without is missing or could not
        // be created, in which case Render() must abandon the frame.
        bool EnsureAndImportFrameTargets(UINT backIndex);
        // Pushes this frame's settings into the screen-space post-process passes (light shafts,
        // lens flare, FXAA), inserting each into the pass registry on first use
        // (RendererPostProcessPasses.cpp). Configuring passes from settings is a separate
        // responsibility from importing resources, and it touches a different set of state
        // (the pass registry, the pass objects and the SRV allocator -- never the render graph).
        void ConfigurePostProcessPasses(UINT backIndex);
        RenderPassExecutionPolicy BuildRenderPassExecutionPolicy(bool useShadowTessPath);
        RenderPassFrameInputs BuildRenderPassFrameInputs(CommandList* cmdList,
                                                         IRhiCommandEncoder* commandEncoder,
                                                         RendererFrameCoordinator::FrameContext* frame,
                                                         D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                         GpuDescriptorHandle defaultAoSrv);
        RenderPassExecutionServices BuildRenderPassExecutionServices(const DrawSceneItemsCallback& drawItems,
                                                                     const DrawShadowItemsCallback& drawShadowItems,
                                                                     bool useMeshShader);
        bool HasRenderPass(std::string_view tag) const;
        void EnsureVolumetricCloudPassInserted();
        // Inserts the mesh-shader (virtual-geometry cluster) pass into the graph right after
        // OpaqueGBuffer if the mesh-shader PSO/root signature are available and not already
        // present. Idempotent; safe to call before init (no-op).
        void EnsureMeshShaderPassInserted();
        // Inserts the debug probe-grid pass into the graph after Skybox/Lighting if it is
        // initialized and not already present. Idempotent; safe to call before init (no-op).
        void EnsureDebugProbeGridPassInserted();
        // Inserts the fluid surface pass into the graph after Skybox/Lighting if it is
        // initialized and not already present. Idempotent; safe to call before init (no-op).
        void EnsureFluidSurfacePassInserted();
        void EnsureFluidBackdropCopyInserted();
        void EnsureSmokeVolumePassInserted();
        void EnsureParticleVolumePassInserted();
        void EnsureParticlePassInserted();
        void EnsureCrowdPassInserted();
        void EnsureWireframeOverlayPassInserted();

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
        // Advances a requested GI probe bake by one frame, from Render()'s tail
        // (RendererGIBakeStep.cpp): the two waiting states (probe grid, SWRT BVH), the SWRT BVH
        // update the probe rays trace against, the probe dispatch itself, the progress logging
        // and the continuous-mode restart. This is a state machine stepped once per frame, not
        // a render stage -- it decides what the bake does next from the bake's own state, not
        // from the frame being drawn -- so it sits apart from the frame-graph code. It is also
        // apart from RendererGI.cpp, which owns the request/status/cache half that runs only
        // when asked and never during a frame. A no-op unless a bake is currently requested.
        void StepGIBake(CommandList* cmdList);
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
        std::shared_ptr<MeshShaderRenderPass> m_meshShaderRenderPass;
        ClusterDrawStats m_lastClusterDrawStats{};
        ClusterDrawStats m_lastClusterShadowStats{};
        DebugVisualizationSystem m_debugVisualization;
        std::shared_ptr<FluidSurfaceRenderPass> m_fluidSurfaceRenderPass;
        // Snapshots SceneColor for the water to refract; inserted alongside the fluid pass.
        std::shared_ptr<FluidBackdropCopyRenderPass> m_fluidBackdropCopyRenderPass;
        std::shared_ptr<SmokeVolumeRenderPass> m_smokeVolumeRenderPass;
        std::shared_ptr<ParticleVolumeRenderPass> m_particleVolumeRenderPass;
        FluidHeightfieldSim m_fluidSim;
        SmokeVolumeSim m_smokeSim;
        std::shared_ptr<ParticleRenderPass> m_particleRenderPass;
        ParticleSystem m_particleSystem;
        std::shared_ptr<CrowdRenderPass> m_crowdRenderPass;
        std::shared_ptr<WireframeOverlayRenderPass> m_wireframeOverlayRenderPass;
        Crowd::CrowdSystem m_crowdSystem;

        SrvDescriptorAllocator m_srvAllocator;
        BloomRenderer m_bloomRenderer;
        ClusterGpuCulling m_clusterGpuCulling;
        ClusterInstanceBuffer m_clusterInstanceBuffer;
        ClusterMaterialTable m_clusterMaterialTable;
        ClusterStreamingFeedback m_clusterStreamingFeedback;
        ClusterFeedbackReadout m_clusterFeedbackScratch;
        ClusterResidencyManager m_clusterResidency;
        ClusterPagePool m_clusterPagePool;
        // Owns the once-per-frame sequencing of the four above (instance refresh, residency
        // sizing, page-pool upload, feedback frame) and the state that sequencing needs -- the
        // offset-table VA every GPU-driven view binds, and the frame/group counters that make
        // the step idempotent. See RendererClusterFrame.cpp for where it is driven from.
        ClusterFrameUpdate m_clusterFrameUpdate;
        GpuDescriptorHandle m_nullTextureSrv{};
        Texture* m_defaultAlbedoTexture     = nullptr;
        Texture* m_defaultOcclusionTexture  = nullptr;
        Texture* m_defaultNormalTexture     = nullptr;
        Texture* m_defaultEmissiveTexture   = nullptr; // white: emissive = material factor alone
        Texture* m_toonFaceShadowMap        = nullptr; // SDF face-shadow threshold map (t19), optional
        std::vector<std::unique_ptr<Texture>> m_defaultTextures; // owns default albedo/AO fallbacks
        Viewport m_viewport{};
        Rect m_scissorRect{};
        bool m_comInitialized = false;

        CameraState m_cameraState;

        RenderTargetPool m_renderTargetPool;
        
        // Debug screenshot state. m_screenshotRequested means "record a copy this frame";
        // m_screenshotCopyRecorded means "a copy is in the submitted command list and the
        // PNG still has to be written". Both are main-thread only.
        TextureCapture m_textureCapture;
        std::wstring      m_screenshotPath;
        std::string       m_screenshotResult;
        // Name of the capture target to resolve at record time (e.g. "backbuffer", "shadow0",
        // "gbuffer.albedo"). Resolving eagerly and holding a resource pointer would go stale
        // across a resize.
        std::string       m_captureTargetName;
        bool              m_screenshotRequested    = false;
        bool              m_screenshotCopyRecorded = false;
        bool              m_screenshotResultReady  = false;

        // Per-pass GPU timing. The counter is monotonic (the swap-chain back-buffer index
        // is not: it wraps every GetBackBufferCount() frames, which would alias distinct
        // frames onto the same profiler ring slot).
        GpuTimestampProfiler m_gpuTimestampProfiler;
        uint64_t             m_gpuProfilerFrameCounter = 0;
        // Render() calls on every backend (m_gpuProfilerFrameCounter counts DX12 frames only). Tells the dynamic
        // vertex rings which writes belong to the same frame.
        uint64_t             m_renderCallCounter = 0;

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
        // Whether the swap chain is *actually* running HDR10 right now, as decided by the last
        // Renderer::ResizeViewport call (m_settings.enableHdrOutput alone can lag this by one
        // resize -- see the comment there). ToneMapSceneColor reads this, not the raw setting, so
        // the tone-map shader's encode path always matches the backbuffer's real format.
        bool m_hdrOutputActive = false;
        bool m_skipShadowCasters = false;
        float m_deltaTime = 0.0f;
        float m_sceneTime = 0.0f;
        GraphicsRuntime m_graphicsRuntime = GetBuildDefaultGraphicsRuntime();
        IRhiFrameOverlay* m_frameOverlay = nullptr;
        FrameComputeHook m_frameComputeHook;
        // Last logged camera signature, so the view-projection is reported when it changes rather
        // than every frame.
        std::string m_lastLoggedCameraSignature;
        BackendFeatureSupport m_featureSupport{};
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

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
