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
#include "Renderer/Passes/Geometry/OpaqueRenderPass.h"
#include "Renderer/Passes/Lighting/LightingRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentLightingRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentBackfaceDistanceRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentCompositeRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentSceneColorCopyRenderPass.h"
#include "Renderer/Passes/Sky/SkyboxRenderPass.h"
#include "Renderer/Passes/PostProcess/PostProcessRenderPass.h"
#include "Renderer/Passes/RayTracing/RayTracingRenderPass.h"
#include "Renderer/Passes/Lighting/SSAORenderPass.h"
#include "Renderer/Passes/Reflections/SoftwareReflectionCompositeRenderPass.h"
#include "Renderer/Passes/Reflections/SoftwareReflectionRenderPass.h"
#include "Renderer/Passes/Sky/ProceduralSkyRenderPass.h"
#include "Renderer/Passes/Sky/VolumetricCloudRenderPass.h"
#include "Renderer/Passes/Debug/DebugProbeGridRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/RayTracing/DxrRayTracer.h"
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include "Renderer/GI/IrradianceProbeGrid.h"
#include "Renderer/Scene/Skybox.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Structures/RendererEnums.h"
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SasamiRenderer
{
    class Renderer
    {
    public:

#pragma region using define

        using RasterShaderMode = RendererEnums::RasterShaderMode;
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

#pragma endregion

        Renderer();
        ~Renderer();

        bool Initialize(HWND hWnd, UINT width, UINT height);
        void Render(const OverlayRenderCallback& overlay = {});
        void WaitForGPU();
        void UpdateCameraCB(const RenderCameraProxy* camera);
        void SubmitRenderProxies(std::vector<RenderProxy>&& proxies);
        void SubmitSkinnedRenderProxies(std::vector<SkinnedRenderProxy>&& proxies);
        void ClearSubmittedRenderProxies();
        void ClearRenderObjects();
        void SetDeltaTime(float dt) { m_deltaTime = dt; }
        void SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height);
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
        RasterShaderMode GetRasterShaderMode() const { return m_settings.rasterShaderMode; }
        void SetRasterShaderMode(RasterShaderMode mode) { m_settings.rasterShaderMode = mode; }
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
        void  RequestGIBake()           { m_probeGrid.ResetBakeState(); m_giBakeRequested = true; }
        void  ResetAndRebakeGI();
        void  CancelGIBake()            { m_giBakeRequested = false; }
        bool  IsGIBaking()        const { return m_giBakeRequested; }
        bool  IsGIBaked()         const { return m_probeGrid.IsBaked(); }
        float GetGIBakeProgress() const { return m_probeGrid.GetBakeProgress(); }

        // Fits the probe grid to the given world AABB and reallocates the buffer.
        // Safe to call after Initialize()  Euse before first rendered frame.
        void FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                  float bMaxX, float bMaxY, float bMaxZ,
                                  float margin = 1.0f);
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

        bool GetDebugProbeGridEnabled()      const { return m_debugProbeGridRenderPass ? m_debugProbeGridRenderPass->IsEnabled() : false; }
        void SetDebugProbeGridEnabled(bool e)      { if (m_debugProbeGridRenderPass) m_debugProbeGridRenderPass->SetEnabled(e); }
        float GetDebugProbeRadius()          const { return m_debugProbeGridRenderPass ? m_debugProbeGridRenderPass->GetProbeRadius() : 0.2f; }
        void SetDebugProbeRadius(float r)          { if (m_debugProbeGridRenderPass) m_debugProbeGridRenderPass->SetProbeRadius(r); }
        // Re-inserts the debug probe grid node into the current pass list.
        // Call after ClearRenderPasses()+AddRenderPass() sequences (e.g. RayMarchApp).
        void ReinsertDebugProbeGrid();
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
        void ApplySwrtModeSetting()
        {
            using SwrtMode = GpuSoftwareRayTracer::SwrtMode;
            m_gpuSoftwareRayTracer.SetMode(m_settings.swrtUseReSTIR ? SwrtMode::ReSTIR : SwrtMode::Legacy);
        }
        RenderPassExecutionPolicy BuildRenderPassExecutionPolicy(bool executeOpaqueFamilyPasses,
                                                                 bool executeLightingFamilyPasses,
                                                                 bool useShadowTessPath);
        RenderPassFrameInputs BuildRenderPassFrameInputs(CommandList* cmdList,
                                                         IRhiCommandEncoder* commandEncoder,
                                                         RendererFrameCoordinator::FrameContext* frame,
                                                         D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                         GpuDescriptorHandle defaultAoSrv);
        RenderPassExecutionServices BuildRenderPassExecutionServices(const DrawSceneItemsCallback& drawItems,
                                                                     const DrawShadowItemsCallback& drawShadowItems);
        bool HasRenderPass(std::string_view tag) const;
        void EnsureVolumetricCloudPassInserted();

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
        void SubmitAndPresent(CommandList* cmdList, UINT frameIndex);
        Texture* CreateTextureFromRgba8Data(const CpuTextureRgba8& src, CommandList* cmdList,
                                            std::vector<Resource>& uploads);
        std::unique_ptr<IRHIDevice> m_device;
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
        std::shared_ptr<OpaqueRenderPass> m_opaqueRenderPass;
        std::shared_ptr<LightingRenderPass> m_lightingRenderPass;
        std::shared_ptr<TransparentRenderPass> m_transparentRenderPass;
        std::shared_ptr<TransparentLightingRenderPass> m_transparentLightingRenderPass;
        std::shared_ptr<TransparentBackfaceDistanceRenderPass> m_transparentBackfaceDistanceRenderPass;
        std::shared_ptr<TransparentCompositeRenderPass> m_transparentCompositeRenderPass;
        std::shared_ptr<TransparentSceneColorCopyRenderPass> m_transparentSceneColorCopyRenderPass;
        std::shared_ptr<SkyboxRenderPass> m_skyboxRenderPass;
        std::shared_ptr<PostProcessRenderPass> m_postProcessRenderPass;
        std::shared_ptr<RayTracingRenderPass> m_rayTracingRenderPass;
        std::shared_ptr<SSAORenderPass> m_ssaoRenderPass;
        std::shared_ptr<SSAOBlurRenderPass> m_ssaoBlurRenderPass;
        std::shared_ptr<SoftwareReflectionRenderPass> m_softwareReflectionRenderPass;
        std::shared_ptr<SoftwareReflectionCompositeRenderPass> m_softwareReflectionCompositeRenderPass;
        std::shared_ptr<ProceduralSkyRenderPass> m_proceduralSkyRenderPass;
        std::shared_ptr<VolumetricCloudRenderPass> m_volumetricCloudRenderPass;
        std::shared_ptr<DebugProbeGridRenderPass> m_debugProbeGridRenderPass;

        SrvDescriptorAllocator m_srvAllocator;
        GpuDescriptorHandle m_nullTextureSrv{};
        Texture* m_defaultAlbedoTexture     = nullptr;
        Texture* m_defaultOcclusionTexture  = nullptr;
        std::vector<std::unique_ptr<Texture>> m_defaultTextures; // owns default albedo/AO fallbacks
        Viewport m_viewport{};
        Rect m_scissorRect{};
        bool m_comInitialized = false;

        CameraState m_cameraState;

        RenderTargetPool m_renderTargetPool;

        std::vector<DeferredUploadBatch> m_deferredUploadBatches;
        RayTracingScene m_rayTracingScene;
        GpuSoftwareRayTracer m_gpuSoftwareRayTracer;
        IrradianceProbeGrid m_probeGrid;
        bool     m_giBakeRequested    = false;
        bool     m_giBakeClearPending = false;
        uint32_t m_giBakeFrameIndex   = 0u;
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

        // Composed subsystems  Emust be declared after all members they reference.
        SceneSynchronizer  m_sceneSynchronizer;
        EnvironmentManager m_environmentManager;
    };
}
