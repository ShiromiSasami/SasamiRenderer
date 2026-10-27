#pragma once

#include <wtypes.h>
#include "IApplication.h"
#include "Light/DirectionalLight.h"
#include "Object/SObject.h"
#include "Object/StaticModel.h"
#include "Object/SkinnedModel.h"
#include "Object/PointLight.h"
#include "Object/SpotLight.h"
#include "ECS/EcsRegistry.h"
#include "Renderer/Config/BackendFeatureSupport.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Structures/RendererEnums.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Foundation layer types: declared in their own namespace so these stay the same entities as
// the definitions, not new SasamiEngine:: types that happen to share a name.
namespace SasamiEngine::Foundation { class InitTaskScheduler; struct BootStatus; }
// Renderer layer types, for the same reason as the Foundation ones above.
namespace SasamiEngine::Render { class IRenderNode; class IRenderPass; struct RenderCameraProxy; }
namespace SasamiEngine::Render::Voxel { class SceneVoxelization; }

namespace SasamiEngine::App
{
    namespace Debug { class DebugRemoteControlServer; class DebugCommandRegistry; class VoxelOccupancyViewer; class WindFlowOverlay; class WindEffectsOverlay; class FrameRecorder; class VoxelStreamOverlay; class ParticleCharacterOverlay; class SmokeSurfaceOverlay; }
    class EditorWindows;

    class AsyncAssetLoadService;
    class Camera;
    class StartupCoordinator;

	class ApplicationCore
	{
    public:
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;
        using RenderPassType = RendererEnums::RenderPassType;
        using RenderPathMode = RendererEnums::RenderPathMode;

        ApplicationCore(UINT width, UINT height, const wchar_t* title, IApplication* game = nullptr);
        ~ApplicationCore();

        int Run();

        // ---- Progressive boot entry points (called only by StartupCoordinator) ----
        // Sequencing lives in StartupCoordinator (AppFramework/Boot); these methods keep
        // ApplicationCore as the single facade over Renderer/ImGui lifetime.
        bool BootCreateRendererCore();                 // Renderer + Renderer::InitializeCore
        bool BootCanRunFrameInfraOnWorker() const;     // backend supportsThreadedResourceCreation
        bool BootInitializeFrameInfrastructure();      // worker-safe when the above is true
        bool BootFinalizeRendererCore();               // FinalizeCoreInitialization + ImGui init
        void BootRegisterDeferredTasks(InitTaskScheduler& scheduler);
        void BootInvokeGameOnInit();                   // runs the game's OnInit exactly once
        void BootRenderLoadingFrame(const BootStatus& status); // boot frame + loading UI
        void SetWindowStatusText(const wchar_t* text); // title-bar progress fallback

        void SetGame(IApplication* game) { m_game = game; }
        HWND GetHwnd() const { return m_hwnd; }
        UINT GetWidth() const { return m_width; }
        UINT GetHeight() const { return m_height; }
        float GetDeltaTime() const { return m_deltaTime; }
        // Deterministic capture (G1): a positive value replaces the wall-clock delta with a
        // constant, so time-driven state (animation, particles, sun) depends on the frame count
        // alone. 0 restores the wall clock. Seeded from SASAMI_FIXED_DT at startup.
        void SetFixedDeltaTime(float seconds) { m_fixedDeltaTime = seconds > 0.0f ? seconds : 0.0f; }
        float GetFixedDeltaTime() const { return m_fixedDeltaTime; }
        // Scene frames rendered since boot handed off; lets a capture wait for N frames
        // instead of a wall-clock sleep.
        uint64_t GetSceneFrameCount() const { return m_sceneFrameCount; }
        bool IsBootRunning() const;
        void RequestQuit() { m_running = false; }
        void SetGraphicsRuntime(GraphicsRuntime runtime);
        GraphicsRuntime GetGraphicsRuntime() const;
        bool IsRendererReady() const;
        Renderer& GetRenderer() { return *m_renderer; }
        void RenderFrame();
        void RenderFrame(const Camera& camera);
        void ResizeRenderer(UINT width, UINT height);

        // Debug remote-control command registry, or null when SASAMI_DEBUG_REMOTE was not
        // set at startup. Exposed so an application can register commands for state that
        // lives in the application rather than here -- the PBR sample's gizmo toggles are
        // sample state, and pushing them into ApplicationCore just to make them scriptable
        // would put sample concerns in the framework.
        //
        // Register only from the main thread. DebugCommandRegistry has no internal
        // synchronization; it is safe because the transport thread only enqueues request
        // lines and DebugRemoteControlServer::DrainPendingCommands is what reads the map,
        // on the main thread. Registering from any other thread races with that drain.
        Debug::DebugCommandRegistry* GetDebugCommandRegistry();

        // Latest scene occupancy voxelization, shared by the voxel.scene command and the
        // "Voxel Occupancy" window. Main thread only.
        Render::Voxel::SceneVoxelization& GetSceneVoxelization() { return *m_sceneVoxelization; }
        Debug::VoxelOccupancyViewer& GetVoxelViewer() { return *m_voxelViewer; }
        Debug::WindFlowOverlay& GetWindFlowOverlay() { return *m_windFlowOverlay; }
        // Smoke / fire / petals carried by the wind overlay's field (voxel.wind.fx). Main thread only.
        Debug::WindEffectsOverlay& GetWindEffectsOverlay() { return *m_windEffectsOverlay; }
        // PNG-sequence recorder behind debug.record. Main thread only.
        Debug::FrameRecorder& GetFrameRecorder() { return *m_frameRecorder; }
        // Camera-centred voxel chunk streaming behind voxel.stream. Main thread only.
        Debug::VoxelStreamOverlay& GetVoxelStreamOverlay() { return *m_voxelStreamOverlay; }
        Debug::ParticleCharacterOverlay& GetParticleCharacterOverlay() { return *m_particleCharacterOverlay; }
        // Live smoke iso-surface behind smoke.mesh live. Main thread only.
        Debug::SmokeSurfaceOverlay& GetSmokeSurfaceOverlay() { return *m_smokeSurfaceOverlay; }
        EditorWindows& GetEditorWindows() { return *m_editorWindows; }

        DirectionalLight GetDirectionalLight() const;
        void SetDirectionalLight(const DirectionalLight& light);
        bool GetShowDirectionalLightOnSkybox() const;
        void SetShowDirectionalLightOnSkybox(bool enabled);
        float GetDirectionalLightOnSkyboxAngularRadius() const;
        void SetDirectionalLightOnSkyboxAngularRadius(float radians);

        float GetIblIntensity() const;
        void SetIblIntensity(float intensity);
        bool GetUseTessellation() const;
        void SetUseTessellation(bool enabled);
        bool GetTessWireframeEnabled() const;
        void SetTessWireframeEnabled(bool enabled);
        bool GetTessDebugColorsEnabled() const;
        void SetTessDebugColorsEnabled(bool enabled);
        bool  GetVolumetricCloudEnabled() const;
        void  SetVolumetricCloudEnabled(bool enabled);
        float GetCloudCover() const;
        void  SetCloudCover(float v);
        float GetCloudDensity() const;
        void  SetCloudDensity(float v);
        float GetCloudWindSpeed() const;
        void  SetCloudWindSpeed(float v);
        float GetCloudBaseAlt() const;
        void  SetCloudBaseAlt(float v);
        float GetCloudTopAlt() const;
        void  SetCloudTopAlt(float v);
        bool GetDebugProbeGridEnabled() const;
        void SetDebugProbeGridEnabled(bool enabled);
        float GetDebugProbeRadius() const;
        void SetDebugProbeRadius(float radius);
        void RequestGIBake();
        void ResetAndRebakeGI();
        void CancelGIBake();
        Renderer::GIBakeStatus GetGIBakeStatus() const;
        float GetGIBakeProgress() const;
        void FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                  float bMaxX, float bMaxY, float bMaxZ,
                                  float margin = 1.0f);
        // World AABB over the loaded scene. False while nothing is loaded yet.
        bool GetSceneWorldBounds(float outMin[3], float outMax[3]) const;
        // Fits the GI probe grid to the whole loaded scene within a probe-count budget.
        bool FitProbeGridToSceneAuto(float margin = 2.0f, std::uint32_t probeBudget = 16384u);
        void ReinsertDebugProbeGrid();
        bool  GetFluidSurfaceEnabled() const;
        void  SetFluidSurfaceEnabled(bool enabled);
        void  SetFluidGridOrigin(float x, float z);
        void  SetFluidCellSize(float cellSize);
        void  SetFluidGridCount(uint32_t countX, uint32_t countZ);
        void  FitFluidToSceneBounds(float minX, float minZ, float maxX, float maxZ, float margin = 1.0f);
        float GetFluidSurfaceHeight() const;
        void  SetFluidSurfaceHeight(float y);
        float GetFluidWaveSpeed() const;
        void  SetFluidWaveSpeed(float v);
        float GetFluidDamping() const;
        void  SetFluidDamping(float d);
        bool  GetFluidSimEnabled() const;
        void  SetFluidSimEnabled(bool enabled);
        void  InjectFluidSplash(float worldX, float worldZ, float radius = 0.5f, float strength = 1.0f);
        void  ReinsertFluidSurface();

        bool  GetParticlesEnabled() const;
        void  SetParticlesEnabled(bool enabled);
        float GetParticleEmissionRate() const;
        void  SetParticleEmissionRate(float rate);
        float GetParticleGravity() const;
        void  SetParticleGravity(float g);
        float GetParticleDrag() const;
        void  SetParticleDrag(float d);
        void  SetParticleEmitOrigin(float x, float y, float z);
        void  ReinsertParticles();
        bool GetUseMeshShader() const;
        void SetUseMeshShader(bool enabled);
        int GetRenderPathModeIndex() const;
        void SetRenderPathModeIndex(int modeIndex);
        int GetRayTracingPerformancePresetIndex() const;
        void SetRayTracingPerformancePresetIndex(int presetIndex);
        bool GetRayTracingDynamicResolutionEnabled() const;
        void SetRayTracingDynamicResolutionEnabled(bool enabled);
        int GetRayTracingMaxBounceCount() const;
        void SetRayTracingMaxBounceCount(int count);
        bool GetRasterSoftwareRayTracedDirectionalShadowEnabled() const;
        void SetRasterSoftwareRayTracedDirectionalShadowEnabled(bool enabled);
        bool GetRasterSoftwareRayTracedReflectionEnabled() const;
        void SetRasterSoftwareRayTracedReflectionEnabled(bool enabled);
        bool GetRasterScreenSpaceReflectionEnabled() const;
        void SetRasterScreenSpaceReflectionEnabled(bool enabled);
        float GetSSRMaxDistance() const;
        void SetSSRMaxDistance(float v);
        float GetSSRThickness() const;
        void SetSSRThickness(float v);
        float GetSSRStepCount() const;
        void SetSSRStepCount(float v);
        float GetSSRRoughnessCutoff() const;
        void SetSSRRoughnessCutoff(float v);
        float GetSSRRefineSteps() const;
        void SetSSRRefineSteps(float v);
        float GetSSREdgeFade() const;
        void SetSSREdgeFade(float v);
        float GetSSRNormalOffset() const;
        void SetSSRNormalOffset(float v);
        float GetSSRIntensity() const;
        void SetSSRIntensity(float v);
        float GetExposure() const;
        void SetExposure(float v);
        bool GetRasterSoftwareRayTracedAmbientOcclusionEnabled() const;
        void SetRasterSoftwareRayTracedAmbientOcclusionEnabled(bool enabled);
        int GetAmbientOcclusionModeIndex() const;
        void SetAmbientOcclusionModeIndex(int modeIndex);
        int GetRuntimeAOMethodIndex() const;
        void SetRuntimeAOMethodIndex(int methodIndex);
        bool GetRuntimeAOEnabled() const;
        void SetRuntimeAOEnabled(bool enabled);
        float GetRuntimeAORadius() const;
        void SetRuntimeAORadius(float radius);
        float GetRuntimeAOBias() const;
        void SetRuntimeAOBias(float bias);
        float GetRuntimeAOIntensity() const;
        void SetRuntimeAOIntensity(float intensity);
        float GetRuntimeAOThickness() const;
        void SetRuntimeAOThickness(float thickness);
        int GetRuntimeAOQualityIndex() const;
        void SetRuntimeAOQualityIndex(int qualityIndex);
        int GetSwrtAoSampleCount() const;
        void SetSwrtAoSampleCount(int count);
        bool GetVsmBlurEnabled() const;
        void SetVsmBlurEnabled(bool enabled);
        float GetAoMinOcclusion() const;
        void  SetAoMinOcclusion(float v);
        float GetAoDirectLightingStrength() const;
        void  SetAoDirectLightingStrength(float v);
        bool GetSwrtUseReSTIR() const;
        void SetSwrtUseReSTIR(bool useReSTIR);
        int  GetSwrtSamplingMode() const;
        void SetSwrtSamplingMode(int mode);
        int  GetSwrtSamplesPerPixel() const;
        void SetSwrtSamplesPerPixel(int n);
        int  GetSwrtMaxBounces() const;
        void SetSwrtMaxBounces(int n);
        bool GetSwrtDenoiserEnabled() const;
        void SetSwrtDenoiserEnabled(bool enabled);
        int  GetSwrtReflectionAtrousIterations() const;
        void SetSwrtReflectionAtrousIterations(int n);
        bool IsHardwareRayTracingSupported() const;
        // Backend/GPU feature availability, for greying out controls the current backend cannot run.
        const BackendFeatureSupport& GetFeatureSupport() const;
        Renderer::RayTracingStats GetRayTracingStats() const;
        int GetGBufferDebugViewIndex() const;
        void SetGBufferDebugViewIndex(int modeIndex);
        void CycleGBufferDebugView(int delta = 1);

        bool LoadSkybox(const std::string& resourcePath, SkyboxLoadFormat format = SkyboxLoadFormat::Auto);
        // HDR-equirect skyboxes: decode + IBL generation run on a JobSystem worker and the
        // sky/IBL pop in when ready. Non-HDR formats fall back to the synchronous LoadSkybox.
        bool LoadSkyboxAsync(const std::string& resourcePath, SkyboxLoadFormat format = SkyboxLoadFormat::Auto);
        AsyncAssetLoadService& GetAssetLoadService();
        std::vector<RenderPassType> GetRenderPassSequence() const;
        void SetRenderPassSequence(const std::vector<RenderPassType>& sequence);
        bool AddRenderNode(const std::shared_ptr<IRenderNode>& renderNode);
        void SetRenderNodePreset(const std::shared_ptr<IRenderNode>& renderNode);
        void UseDefaultRenderNodePreset();
        // Default preset plus the toon outline pass. Opt-in: the outline is not part of
        // the default sequence, so other samples pay nothing for it.
        void UseToonRenderNodePreset();
        bool AddRenderPass(const std::shared_ptr<IRenderPass>& renderPass);
        bool AddRenderPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        bool AddRenderPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        bool ReplaceRenderPass(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        void ClearRenderPasses();

        template<typename TObject, typename... TArgs>
        TObject* CreateObject(TArgs&&... args)
        {
            static_assert(std::is_base_of_v<SObject, TObject>, "TObject must derive from SObject.");

            std::unique_ptr<TObject> object = std::make_unique<TObject>(std::forward<TArgs>(args)...);
            if (!object) {
                return nullptr;
            }

            TObject* created = object.get();
            m_objects.push_back(std::move(object));
            RegisterObjectInEcs(created);
            m_objectsDirty = true;
            return created;
        }

        StaticModel* CreateStaticModel()
        {
            return CreateObject<StaticModel>();
        }
        SkinnedModel* CreateSkinnedModel()
        {
            return CreateObject<SkinnedModel>();
        }
        PointLight* CreatePointLightObject() { return CreateObject<PointLight>(); }
        SpotLight* CreateSpotLightObject() { return CreateObject<SpotLight>(); }
        std::vector<PointLight*> GetPointLightObjects() const;
        std::vector<SpotLight*> GetSpotLightObjects() const;
        std::vector<StaticModel*> GetStaticModels() const;
        std::vector<SkinnedModel*> GetSkinnedModels() const;
        std::vector<Camera*> GetCameraObjects() const;
        bool SaveScene(const std::string& path) const;
        bool LoadScene(const std::string& path);

        // --- Debug screenshot -------------------------------------------------------
        // Queues a capture of the renderer's back buffer. `path` is UTF-8 and is resolved
        // to an absolute path (returned via outResolvedPath) so the caller is told exactly
        // where the file will appear regardless of the process's working directory.
        // The capture completes during a later frame; poll PollScreenshotResult for it.
        bool RequestScreenshot(const std::string& path, std::string& outResolvedPath);
        // True once the queued capture finished, with `outMessage` describing the outcome.
        bool PollScreenshotResult(std::string& outMessage);
        // Queues a dump of an intermediate render target (e.g. "gbuffer.albedo") to a PNG.
        // `path` is UTF-8 and is resolved the same way as RequestScreenshot. Returns an
        // empty string when the request was accepted, or an error message otherwise.
        // Shares the same pending slot as RequestScreenshot; poll PollScreenshotResult
        // for completion.
        std::string RequestTargetDump(const std::string& name, const std::string& path);
        // Non-destructive: applies [camera]/[directional_light]/[point_light]/[spot_light]
        // onto existing objects without clearing the scene. Ignores [static_model] and
        // unknown sections. Intended for restoring saved view/light state at startup.
        bool ApplyCameraAndLights(const std::string& path);
        Camera* CreateCameraObject();
        bool SetMainCamera(Camera* camera);
        Camera* GetMainCamera() const { return m_activeCamera; }
        const RenderCameraProxy& GetMainCameraProxy() const { return m_activeCameraProxy; }
        Camera* GetActiveCamera() const { return m_activeCamera; }
        bool DeleteObject(SObject* object);
        void ClearObjects();
        // Bumped by every ClearObjects. Raw SObject pointers a game keeps across frames are
        // dangling once it changes -- LoadScene clears and rebuilds the scene from the
        // debug pipe while the game still holds its camera and model pointers.
        uint64_t GetSceneGeneration() const { return m_sceneGeneration; }
        void InvalidateRenderObjects() { m_objectsDirty = true; }

    private:
        using EntityId = EcsRegistry::EntityId;
        struct ObjectRefComponent
        {
            SObject* object = nullptr;
        };

        void InitializeDebugRemoteControl();
        bool InitializeRenderer();
        // Shared by InitializeRenderer/BootFinalizeRendererCore: stands up ImGui's renderer
        // backend (or falls back to platform-only input) and, if it succeeds, wires the
        // resulting overlay recorder into the renderer.
        bool InitializeUIOverlay();
        void ShutdownRenderer();
        // Shared by RequestScreenshot/RequestTargetDump: resolves `path` to an absolute
        // path (falling back to the input path if resolution fails) and ensures a ".png"
        // extension, returning both the UTF-8 and wide-string forms. False if `path` is empty.
        static bool ResolveCapturePath(const std::string& path, std::string& outResolvedPath, std::wstring& outWidePath);
        void RenderFrameInternal(const RenderCameraProxy& cameraProxy);
        bool UpdateMainCameraProxy();
        void SyncModelsToRenderer(Renderer& renderer);
        void SyncSkinnedModelsToRenderer(Renderer& renderer);
        void SyncLightObjectsToRenderer(Renderer& renderer) const;
        void RegisterObjectInEcs(SObject* object);
        void UnregisterObjectInEcs(SObject* object);
        void OnInit();
        void OnUpdate(float deltaTime);
        void OnRender();
        void OnDestroy();

    private:
        static LRESULT CALLBACK WindowProccessStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT WindowProccess(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND m_hwnd;
        UINT m_width;
        UINT m_height;
        const wchar_t* m_title;
        bool m_running;

        IApplication* m_game;
        std::unique_ptr<AsyncAssetLoadService> m_assetLoadService;
        std::unique_ptr<StartupCoordinator> m_startup;
        std::unique_ptr<Renderer> m_renderer;
        // Declared after m_renderer so they are destroyed first: the voxelizer owns GPU resources.
        std::unique_ptr<Render::Voxel::SceneVoxelization> m_sceneVoxelization;
        std::unique_ptr<Debug::VoxelOccupancyViewer> m_voxelViewer;
        std::unique_ptr<Debug::WindFlowOverlay> m_windFlowOverlay;
        std::unique_ptr<Debug::WindEffectsOverlay> m_windEffectsOverlay;
        std::unique_ptr<Debug::FrameRecorder> m_frameRecorder;
        std::unique_ptr<Debug::VoxelStreamOverlay> m_voxelStreamOverlay;
        std::unique_ptr<Debug::ParticleCharacterOverlay> m_particleCharacterOverlay;
        std::unique_ptr<Debug::SmokeSurfaceOverlay> m_smokeSurfaceOverlay;
        std::unique_ptr<EditorWindows> m_editorWindows;
        GraphicsRuntime m_graphicsRuntime = GetBuildDefaultGraphicsRuntime();
        float m_deltaTime = 0.0f;
        float m_fixedDeltaTime = 0.0f;
        uint64_t m_sceneFrameCount = 0;
        std::vector<std::unique_ptr<SObject>> m_objects;
        EcsRegistry m_ecsRegistry;
        std::unordered_map<SObject*, EntityId> m_objectEntityMap;
        Camera* m_activeCamera = nullptr;
        RenderCameraProxy m_activeCameraProxy{};
        bool m_objectsDirty = false;
        uint64_t m_sceneGeneration = 0;
        // Debug-only remote control channel. Null unless SASAMI_DEBUG_REMOTE=1 was set at
        // startup. Held by pointer so the header does not have to pull in the transport
        // (and with it <windows.h> networking headers) for every ApplicationCore user.
        std::unique_ptr<Debug::DebugRemoteControlServer> m_debugRemoteControl;
		};
}
namespace SasamiEngine { using namespace App; }
