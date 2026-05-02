#pragma once
#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RayTracingStats.h"
#include "Renderer/Core/RenderSettings.h"
#include "Renderer/Core/RenderTargetPool.h"
#include "Renderer/RayTracing/SWRTExecutor.h"
#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/Core/RenderGraph.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Core/RenderNodeConstants.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Core/SrvDescriptorAllocator.h"
#include "Renderer/Core/CameraState.h"
#include "Renderer/Core/RenderPassRegistry.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/DrawCommandBuilder.h"
#include "Renderer/Passes/ShadowRenderNode.h"
#include "Renderer/Passes/OpaqueRenderNode.h"
#include "Renderer/Passes/LightingRenderNode.h"
#include "Renderer/Passes/TransparentRenderNode.h"
#include "Renderer/Passes/TransparentLightingRenderNode.h"
#include "Renderer/Passes/SkyboxRenderNode.h"
#include "Renderer/Passes/PostProcessRenderNode.h"
#include "Renderer/Passes/RayTracingRenderNode.h"
#include "Renderer/Passes/SSAORenderNode.h"
#include "Renderer/Passes/ProceduralSkyRenderNode.h"
#include "Renderer/Passes/SdfFluidRenderNode.h"
#include "Renderer/Passes/VolumetricCloudRenderNode.h"
#include "Renderer/Passes/DebugProbeGridRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
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
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;
        using RenderNodeType = RendererEnums::RenderNodeType;

        using DirectionalLightSettings = LightSystem::DirectionalLightSettings;
        using PointLight = LightSystem::PointLight;
        using SpotLight = LightSystem::SpotLight;

        using RenderProxy = SasamiRenderer::RenderProxy;

        using OverlayRenderCallback = std::function<void(CommandList&, CpuDescriptorHandle)>;
        using PhaseCompletionMode = RenderGraph::PhaseCompletionMode;
        using PhaseCompletionCallback = std::function<bool(const RenderNodeContextView&)>;

        using RayTracingStats = SasamiRenderer::RayTracingStats;

        using PassHandle = RenderPassRegistry::PassHandle;

#pragma endregion

        Renderer();
        ~Renderer();

        bool Initialize(HWND hWnd, UINT width, UINT height);
        void Render(const OverlayRenderCallback& overlay = {});
        void UpdateCameraCB(const RenderCameraProxy* camera);
        void SubmitRenderProxies(std::vector<RenderProxy>&& proxies);
        void ClearSubmittedRenderProxies();
        void ClearRenderObjects();
        void SetDeltaTime(float dt) { m_deltaTime = dt; }
        void SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height);
        void SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height);
        void SetSkyboxLoadFormat(SkyboxLoadFormat format);
        bool GetShowDirectionalLightOnSkybox() const { return m_skybox.IsDirectionalLightMarkerEnabled(); }
        void SetShowDirectionalLightOnSkybox(bool enabled) { m_skybox.SetDirectionalLightMarkerEnabled(enabled); }
        float GetDirectionalLightOnSkyboxAngularRadius() const { return m_skybox.GetDirectionalLightMarkerAngularRadius(); }
        void SetDirectionalLightOnSkyboxAngularRadius(float radians) { m_skybox.SetDirectionalLightMarkerAngularRadius(radians); }
        PassHandle AddPass(const std::shared_ptr<IRenderNode>& renderPass);
        PassHandle AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        PassHandle AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        bool ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        bool AddPhaseCompletionNode(std::string_view phaseTag,
                                    std::string_view nodeName,
                                    const PhaseCompletionCallback& execute,
                                    PhaseCompletionMode mode = PhaseCompletionMode::Deterministic,
                                    const RenderNodeRequirements& requirements = {});
        void ClearPhaseCompletionNodes();
        void ClearPasses();
        const std::vector<RenderNodeType>& GetRenderNodeSequence() const { return m_passRegistry.GetRenderNodeSequence(); }
        void SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence);
        void RefreshEnvironmentAssets();
        void SetGraphicsRuntime(GraphicsRuntime runtime) { m_graphicsRuntime = runtime; }
        GraphicsRuntime GetGraphicsRuntime() const { return m_graphicsRuntime; }
        void SetRHIBackend(RHIBackend backend) { SetGraphicsRuntime(backend); }
        RHIBackend GetRHIBackend() const { return GetGraphicsRuntime(); }
        void SetGraphicsBackend(GraphicsBackend backend) { SetRHIBackend(backend); }
        GraphicsBackend GetGraphicsBackend() const { return GetRHIBackend(); }

        float GetIblIntensity() const { return m_settings.iblIntensity; }
        void SetIblIntensity(float intensity) { m_settings.iblIntensity = (intensity < 0.0f) ? 0.0f : intensity; }
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
        void SetRenderPathMode(RenderPathMode mode)
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
        }
        RayTracingPerformancePreset GetRayTracingPerformancePreset() const { return m_settings.rayTracingPerformancePreset; }
        void SetRayTracingPerformancePreset(RayTracingPerformancePreset preset) { m_settings.rayTracingPerformancePreset = preset; }
        bool GetRayTracingDynamicResolutionEnabled() const { return m_settings.rayTracingDynamicResolutionEnabled; }
        void SetRayTracingDynamicResolutionEnabled(bool enabled) { m_settings.rayTracingDynamicResolutionEnabled = enabled; }
        uint32_t GetRayTracingMaxBounceCount() const { return m_settings.rayTracingMaxBounceCount; }
        void SetRayTracingMaxBounceCount(uint32_t count)
        {
            if (count < kMinRayTracingBounceCount) {
                count = kMinRayTracingBounceCount;
            }
            if (count > kMaxRayTracingBounceCount) {
                count = kMaxRayTracingBounceCount;
            }
            m_settings.rayTracingMaxBounceCount = count;
        }
        bool GetRasterSoftwareRayTracedDirectionalShadowEnabled() const
        {
            return m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled;
        }
        void SetRasterSoftwareRayTracedDirectionalShadowEnabled(bool enabled)
        {
            if (m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled != enabled) {
                m_swrtExecutor.InvalidateShadowCache();
            }
            m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled = enabled;
        }
        bool GetRasterSoftwareRayTracedReflectionEnabled() const
        {
            return m_settings.rasterSoftwareRayTracedReflectionEnabled;
        }
        void SetRasterSoftwareRayTracedReflectionEnabled(bool enabled)
        {
            if (m_settings.rasterSoftwareRayTracedReflectionEnabled != enabled) {
                m_swrtExecutor.OnReflectionResourcesReallocated();
            }
            m_settings.rasterSoftwareRayTracedReflectionEnabled = enabled;
        }
        bool GetRasterSoftwareRayTracedAmbientOcclusionEnabled() const
        {
            return m_settings.ambientOcclusionMode == RendererEnums::AmbientOcclusionMode::SWRTAOOnly;
        }
        void SetRasterSoftwareRayTracedAmbientOcclusionEnabled(bool enabled)
        {
            m_settings.rasterSoftwareRayTracedAmbientOcclusionEnabled = enabled;
            m_settings.ambientOcclusionMode = enabled
                ? RendererEnums::AmbientOcclusionMode::SWRTAOOnly
                : RendererEnums::AmbientOcclusionMode::Hybrid;
        }
        RendererEnums::AmbientOcclusionMode GetAmbientOcclusionMode() const
        {
            return m_settings.ambientOcclusionMode;
        }
        void SetAmbientOcclusionMode(RendererEnums::AmbientOcclusionMode mode)
        {
            m_settings.ambientOcclusionMode = mode;
            m_settings.rasterSoftwareRayTracedAmbientOcclusionEnabled =
                (mode == RendererEnums::AmbientOcclusionMode::SWRTAOOnly);
        }
        bool GetSwrtUseReSTIR() const { return m_settings.swrtUseReSTIR; }
        void SetSwrtUseReSTIR(bool useReSTIR)
        {
            m_settings.swrtUseReSTIR = useReSTIR;
            using SwrtMode = GpuSoftwareRayTracer::SwrtMode;
            m_gpuSoftwareRayTracer.SetMode(useReSTIR ? SwrtMode::ReSTIR : SwrtMode::Legacy);
        }
        uint32_t GetSwrtSamplingMode() const { return m_settings.swrtSamplingMode; }
        void SetSwrtSamplingMode(uint32_t mode) { m_settings.swrtSamplingMode = (mode < 2u ? mode : 2u); }
        uint32_t GetSwrtSamplesPerPixel() const { return m_settings.swrtSamplesPerPixel; }
        void SetSwrtSamplesPerPixel(uint32_t n) { m_settings.swrtSamplesPerPixel = n; }
        uint32_t GetSwrtMaxBounces() const { return m_settings.swrtMaxBounces; }
        void SetSwrtMaxBounces(uint32_t n) { m_settings.swrtMaxBounces = (n < 1u ? 1u : (n > 8u ? 8u : n)); }
        bool GetGIEnabled()     const { return m_probeGrid.GetEnabled(); }
        void SetGIEnabled(bool e)     { m_probeGrid.SetEnabled(e); }
        float GetGIIntensity()  const { return m_probeGrid.GetGiIntensity(); }
        void SetGIIntensity(float v)  { m_probeGrid.SetGiIntensity(v); }
        float GetGIEmaAlpha()   const { return m_probeGrid.GetEmaAlpha(); }
        void SetGIEmaAlpha(float a)   { m_probeGrid.SetEmaAlpha(a); }
        IrradianceProbeGrid& GetProbeGrid() { return m_probeGrid; }
        // Fits the probe grid to the given world AABB and reallocates the buffer.
        // Safe to call after Initialize() — use before first rendered frame.
        void FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                  float bMaxX, float bMaxY, float bMaxZ,
                                  float margin = 1.0f);
        // Volumetric cloud
        bool  GetVolumetricCloudEnabled()  const { return m_settings.volumetricCloudEnabled; }
        void  SetVolumetricCloudEnabled(bool e)  { m_settings.volumetricCloudEnabled = e; if (m_volumetricCloudRenderNode) m_volumetricCloudRenderNode->SetEnabled(e); }
        float GetCloudCover()   const { return m_settings.cloudCover; }
        void  SetCloudCover(float v) { m_settings.cloudCover = v; if (m_volumetricCloudRenderNode) m_volumetricCloudRenderNode->SetCloudCover(v); if (m_sdfFluidRenderNode) m_sdfFluidRenderNode->SetCloudCover(v); }
        float GetCloudDensity() const { return m_settings.cloudDensity; }
        void  SetCloudDensity(float v) { m_settings.cloudDensity = v; if (m_volumetricCloudRenderNode) m_volumetricCloudRenderNode->SetCloudDensity(v); if (m_sdfFluidRenderNode) m_sdfFluidRenderNode->SetCloudDensity(v); }
        float GetCloudWindSpeed() const { return m_settings.cloudWindSpeed; }
        void  SetCloudWindSpeed(float v) { m_settings.cloudWindSpeed = v; if (m_volumetricCloudRenderNode) m_volumetricCloudRenderNode->SetWindSpeed(v); }
        float GetCloudBaseAlt() const { return m_settings.cloudBaseAlt; }
        void  SetCloudBaseAlt(float v) { m_settings.cloudBaseAlt = v; if (m_volumetricCloudRenderNode) m_volumetricCloudRenderNode->SetCloudBaseAlt(v); }
        float GetCloudTopAlt()  const { return m_settings.cloudTopAlt; }
        void  SetCloudTopAlt(float v) { m_settings.cloudTopAlt = v; if (m_volumetricCloudRenderNode) m_volumetricCloudRenderNode->SetCloudTopAlt(v); }

        bool GetDebugProbeGridEnabled()      const { return m_debugProbeGridRenderNode ? m_debugProbeGridRenderNode->IsEnabled() : false; }
        void SetDebugProbeGridEnabled(bool e)      { if (m_debugProbeGridRenderNode) m_debugProbeGridRenderNode->SetEnabled(e); }
        float GetDebugProbeRadius()          const { return m_debugProbeGridRenderNode ? m_debugProbeGridRenderNode->GetProbeRadius() : 0.2f; }
        void SetDebugProbeRadius(float r)          { if (m_debugProbeGridRenderNode) m_debugProbeGridRenderNode->SetProbeRadius(r); }
        // Re-inserts the debug probe grid node into the current pass list.
        // Call after ClearRenderPasses()+AddRenderPass() sequences (e.g. RayMarchApp).
        void ReinsertDebugProbeGrid();
        GBufferDebugView GetGBufferDebugView() const { return m_settings.gBufferDebugView; }
        void SetGBufferDebugView(GBufferDebugView view)
        {
            if (m_settings.renderPathMode != RenderPathMode::Raster) {
                m_settings.gBufferDebugView = GBufferDebugView::FinalLit;
                return;
            }

            const int index = static_cast<int>(view);
            const int count = static_cast<int>(GBufferDebugView::Count);
            if (index >= 0 && index < count) {
                m_settings.gBufferDebugView = view;
            } else {
                m_settings.gBufferDebugView = GBufferDebugView::FinalLit;
            }
        }
        void CycleGBufferDebugView(int delta = 1)
        {
            if (m_settings.renderPathMode != RenderPathMode::Raster) {
                m_settings.gBufferDebugView = GBufferDebugView::FinalLit;
                return;
            }

            const int count = static_cast<int>(GBufferDebugView::Count);
            if (count <= 0) {
                m_settings.gBufferDebugView = GBufferDebugView::FinalLit;
                return;
            }

            int index = static_cast<int>(m_settings.gBufferDebugView);
            index = (index + delta) % count;
            if (index < 0) {
                index += count;
            }
            m_settings.gBufferDebugView = static_cast<GBufferDebugView>(index);
        }
        float GetDeltaTime() const { return m_deltaTime; }
        bool GetSSAOEnabled() const { return m_settings.ssaoEnabled; }
        void SetSSAOEnabled(bool enabled) { m_settings.ssaoEnabled = enabled; }
        float GetSSAORadius() const { return m_settings.ssaoRadius; }
        void SetSSAORadius(float r) { m_settings.ssaoRadius = (r > 0.0f) ? r : 0.01f; }
        float GetSSAOBias() const { return m_settings.ssaoBias; }
        void SetSSAOBias(float b) { m_settings.ssaoBias = b; }
        float GetSSAOIntensity() const { return m_settings.ssaoIntensity; }
        void SetSSAOIntensity(float i) { m_settings.ssaoIntensity = (i >= 0.0f) ? i : 0.0f; }
        float GetSSAOThickness() const { return m_settings.ssaoThickness; }
        void SetSSAOThickness(float t) { m_settings.ssaoThickness = (t >= 0.0f) ? t : 0.0f; }
        uint32_t GetSSAOQuality() const { return m_settings.ssaoQuality; }
        void SetSSAOQuality(uint32_t q) { m_settings.ssaoQuality = (q > 2u) ? 2u : q; }
        uint32_t GetSwrtAoSampleCount() const { return m_settings.swrtAoSampleCount; }
        void SetSwrtAoSampleCount(uint32_t count)
        {
            if (count < 4u) {
                count = 4u;
            } else if (count > 32u) {
                count = 32u;
            }
            m_settings.swrtAoSampleCount = count;
        }
        float GetAoMinOcclusion() const { return m_settings.aoMinOcclusion; }
        void  SetAoMinOcclusion(float v)
        {
            if (v < 0.0f) {
                v = 0.0f;
            } else if (v > 1.0f) {
                v = 1.0f;
            }
            m_settings.aoMinOcclusion = v;
        }
        DirectionalLightSettings GetDirectionalLightSettings() const;
        void SetDirectionalLightSettings(const DirectionalLightSettings& settings);
        std::vector<PointLight>& GetPointLights() { return m_lightSystem.GetPointLights(); }
        const std::vector<PointLight>& GetPointLights() const { return m_lightSystem.GetPointLights(); }
        std::vector<SpotLight>& GetSpotLights() { return m_lightSystem.GetSpotLights(); }
        const std::vector<SpotLight>& GetSpotLights() const { return m_lightSystem.GetSpotLights(); }
        void* GetNativeDeviceHandle() const { return m_device ? m_device->GetNativeDeviceHandle() : nullptr; }
        void* GetNativeGraphicsQueueHandle() const { return m_device ? m_device->GetNativeGraphicsQueueHandle() : nullptr; }
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
        void EnsureEnvironmentTexturesUploaded(CommandList* cmdList);
        SWRTExecutor::FrameContext BuildSwrtFrameContext() const;
        RenderNodeExecutionPolicy BuildRenderNodeExecutionPolicy(bool executeOpaqueFamilyPasses,
                                                                 bool executeLightingFamilyPasses,
                                                                 bool useShadowTessPath);
        RenderNodeFrameInputs BuildRenderNodeFrameInputs(CommandList* cmdList,
                                                         RendererFrameCoordinator::FrameContext* frame,
                                                         D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                         GpuDescriptorHandle defaultAoSrv);
        RenderNodeExecutionServices BuildRenderNodeExecutionServices(const DrawSceneItemsCallback& drawItems,
                                                                     const DrawShadowItemsCallback& drawShadowItems);

        void TransitionBackBufferToRenderTarget(CommandList* cmdList, UINT backIndex);
        void ClearAndBindMainTargets(CommandList* cmdList, UINT backIndex);
        void BindMainTargets(CommandList* cmdList, UINT backIndex);
        void TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex);
        void SubmitAndPresent(CommandList* cmdList, UINT frameIndex);
        Texture* CreateTextureFromRgba8Data(const CpuTextureRgba8& src, CommandList* cmdList,
                                            std::vector<Resource>& uploads);
        std::unique_ptr<IRHIDevice> m_device;
        RenderPipelineStateCache m_pipelineStateCache;
        RendererFrameCoordinator m_frameCoordinator;
        MeshBuffer m_meshBuffer;
        DrawCommandBuilder m_drawCommandBuilder;
        Skybox m_skybox;
        LightSystem m_lightSystem;
        RenderGraph m_renderGraph;
        std::shared_ptr<ShadowRenderNode> m_shadowRenderNode;
        std::shared_ptr<OpaqueRenderNode> m_opaqueRenderNode;
        std::shared_ptr<LightingRenderNode> m_lightingRenderNode;
        std::shared_ptr<TransparentRenderNode> m_transparentRenderNode;
        std::shared_ptr<TransparentLightingRenderNode> m_transparentLightingRenderNode;
        std::shared_ptr<SkyboxRenderNode> m_skyboxRenderNode;
        std::shared_ptr<PostProcessRenderNode> m_postProcessRenderNode;
        std::shared_ptr<RayTracingRenderNode> m_rayTracingRenderNode;
        std::shared_ptr<SSAORenderNode> m_ssaoRenderNode;
        std::shared_ptr<ProceduralSkyRenderNode> m_proceduralSkyRenderNode;
        std::shared_ptr<SdfFluidRenderNode> m_sdfFluidRenderNode;
        std::shared_ptr<VolumetricCloudRenderNode> m_volumetricCloudRenderNode;
        std::shared_ptr<DebugProbeGridRenderNode> m_debugProbeGridRenderNode;

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
        DxrRayTracer m_dxrRayTracer;
        DxrRayTracer::DescriptorSet m_rayTracingDescriptors{};

        SWRTExecutor  m_swrtExecutor;
        SceneSubmitter m_sceneSubmitter;

        RenderSettings m_settings;
        RayTracingStats m_rayTracingStats{};
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
    };
}
