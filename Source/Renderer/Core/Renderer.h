#pragma once
#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderTargetResourceBinder.h"
#include "Renderer/Core/RenderLayerConfigurator.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/DrawCommandBuilder.h"
#include "Renderer/Scene/Material.h"
#include "Renderer/Structures/RendererEnums.h"
#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace SasamiRenderer
{
    class Renderer
    {
    public:
        using RasterShaderMode = RendererEnums::RasterShaderMode;
        using GBufferDebugView = RendererEnums::GBufferDebugView;
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;

        using DirectionalLightSettings = RenderDirectionalLight;
        using PointLight = RenderPointLight;
        using SpotLight = RenderSpotLight;

        using RenderProxy = SasamiRenderer::RenderProxy;

        using OverlayRenderCallback = std::function<void(CommandList&, CpuDescriptorHandle)>;

        Renderer() = default;
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
        void SetSkyboxLoadFormat(SkyboxLoadFormat format) { m_skyboxLoadFormat = format; }
        void RefreshEnvironmentAssets();
        void SetGraphicsRuntime(GraphicsRuntime runtime) { m_graphicsRuntime = runtime; }
        GraphicsRuntime GetGraphicsRuntime() const { return m_graphicsRuntime; }
        void SetRHIBackend(RHIBackend backend) { SetGraphicsRuntime(backend); }
        RHIBackend GetRHIBackend() const { return GetGraphicsRuntime(); }
        void SetGraphicsBackend(GraphicsBackend backend) { SetRHIBackend(backend); }
        GraphicsBackend GetGraphicsBackend() const { return GetRHIBackend(); }

        float GetIblIntensity() const { return m_iblIntensity; }
        void SetIblIntensity(float intensity) { m_iblIntensity = (intensity < 0.0f) ? 0.0f : intensity; }
        void ResizeViewport(UINT width, UINT height);
        bool GetUseTessellation() const { return m_useTessellation; }
        void SetUseTessellation(bool enable) { m_useTessellation = enable; }
        RasterShaderMode GetRasterShaderMode() const { return m_rasterShaderMode; }
        void SetRasterShaderMode(RasterShaderMode mode) { m_rasterShaderMode = mode; }
        GBufferDebugView GetGBufferDebugView() const { return m_gBufferDebugView; }
        void SetGBufferDebugView(GBufferDebugView view)
        {
            const int index = static_cast<int>(view);
            const int count = static_cast<int>(GBufferDebugView::Count);
            if (index >= 0 && index < count) {
                m_gBufferDebugView = view;
            } else {
                m_gBufferDebugView = GBufferDebugView::FinalLit;
            }
        }
        void CycleGBufferDebugView(int delta = 1)
        {
            const int count = static_cast<int>(GBufferDebugView::Count);
            if (count <= 0) {
                m_gBufferDebugView = GBufferDebugView::FinalLit;
                return;
            }

            int index = static_cast<int>(m_gBufferDebugView);
            index = (index + delta) % count;
            if (index < 0) {
                index += count;
            }
            m_gBufferDebugView = static_cast<GBufferDebugView>(index);
        }
        float GetDeltaTime() const { return m_deltaTime; }
        DirectionalLightSettings GetDirectionalLightSettings() const;
        void SetDirectionalLightSettings(const DirectionalLightSettings& settings);
        std::vector<PointLight>& GetPointLights() { return m_pointLights; }
        const std::vector<PointLight>& GetPointLights() const { return m_pointLights; }
        std::vector<SpotLight>& GetSpotLights() { return m_spotLights; }
        const std::vector<SpotLight>& GetSpotLights() const { return m_spotLights; }
        void* GetNativeDeviceHandle() const { return m_device ? m_device->GetNativeDeviceHandle() : nullptr; }
        void* GetNativeGraphicsQueueHandle() const { return m_device ? m_device->GetNativeGraphicsQueueHandle() : nullptr; }
        ID3D12Device* GetNativeDevice() const { return m_device ? m_device->GetDevice() : nullptr; }
        ID3D12CommandQueue* GetNativeCommandQueue() const { return m_device ? m_device->GetCommandQueue().Get() : nullptr; }
        Format GetBackBufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
        Format GetDepthFormat() const { return DXGI_FORMAT_D32_FLOAT; }
        UINT GetBackBufferCount() const { return 2; }

    private:
        struct DrawItem
        {
            size_t meshIndex = 0;
            Texture* texture = nullptr;
            float model[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        struct FrameContext
        {
            CommandAllocator cmdAllocator;
            Resource cameraCB;
            uint8_t* cameraCBPtr = nullptr;
            UINT cameraCbCapacity = 0;
            UINT cameraCbCount = 0;
            Resource lightCB;
            void* lightCBPtr = nullptr;

            Resource pointLightBuffer;
            Resource spotLightBuffer;
            void* pointLightBufferPtr = nullptr;
            void* spotLightBufferPtr = nullptr;
            UINT pointLightCapacity = 0;
            UINT spotLightCapacity = 0;

            CpuDescriptorHandle pointSrvCpu{};
            CpuDescriptorHandle spotSrvCpu{};
            GpuDescriptorHandle lightSrvTable{};
            UINT64 fenceValue = 0;
        };

        bool InitializeFrameContexts(UINT frameCount);
        bool ResetCommandListForFrame(UINT frameIndex, CommandList*& outCmdList);
        void WaitForFrameFence(UINT frameIndex);
        void SignalFrameFence(UINT frameIndex);
        bool AllocateSrvRange(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu);

        void ShadowPass(CommandList* cmdList, FrameContext& frame);
        void TransitionBackBufferToRenderTarget(CommandList* cmdList, UINT backIndex);
        void ClearAndBindMainTargets(CommandList* cmdList, UINT backIndex);
        void EnsureIconTextureUploaded(CommandList* cmdList);
        void EnsureSkyboxTextureUploaded(CommandList* cmdList);
        void EnsureIblTexturesUploaded(CommandList* cmdList);
        bool EnsureHdrEnvironmentLoaded();
        bool InitializeSkyboxGeometry();
        void MainPass(CommandList* cmdList, FrameContext& frame);
        void SkyboxPass(CommandList* cmdList, FrameContext& frame);
        void TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex);
        void SubmitAndPresent(CommandList* cmdList, UINT frameIndex);
        Texture* CreateTextureFromRgba8Data(const CpuTextureRgba8& src, CommandList* cmdList,
                                            std::vector<Resource>& uploads);
        Texture* ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData);
        void EnsureCameraBuffers(FrameContext& frame, UINT requiredCount);
        D3D12_GPU_VIRTUAL_ADDRESS PushCameraCB(FrameContext& frame, const float mvp[16], const float world[16]);
        void EnsureLightBuffers(FrameContext& frame, size_t pointCount, size_t spotCount);

        std::unique_ptr<IRHIDevice> m_device;
        RenderTargetResourceBinder m_rtBinder;
        RenderLayerConfigurator m_rlConfig;
        std::vector<Mesh> m_meshes;
        MeshBuffer m_meshBuffer;
        DrawCommandBuilder m_drawCommandBuilder;
        Material m_material;
        Texture m_iconTex;
        bool m_iconLoaded = false;
        std::vector<std::unique_ptr<Texture>> m_sceneTextures;
        enum class SkyboxSourceType
        {
            None = 0,
            HdrRgbFloat = 1,
            LdrRgba8 = 2,
        };
        SkyboxSourceType m_skyboxSourceType = SkyboxSourceType::None;
        UINT m_skyboxSourceWidth = 0;
        UINT m_skyboxSourceHeight = 0;
        std::vector<float> m_skyboxSourceHdrRgb;
        std::vector<uint8_t> m_skyboxSourceLdrRgba8;

        std::vector<FrameContext> m_frames;
        CommandList m_mainCommandList;
        bool m_mainCommandListReady = false;
        ComPtr<ID3D12Fence> m_frameFence;
        HANDLE m_frameFenceEvent = nullptr;
        UINT64 m_nextFenceValue = 1;

        DescriptorHeap m_srvHeap;
        UINT m_srvCapacity = 0;
        UINT m_srvNext = 0;
        CpuDescriptorHandle m_iconSrvCpu{};
        CpuDescriptorHandle m_skyboxSrvCpu{};
        GpuDescriptorHandle m_skyboxSrv{};
        CpuDescriptorHandle m_iblSrvCpu{};
        GpuDescriptorHandle m_iblSrv{};

        Resource m_texture;
        Resource m_textureUpload;
        bool m_textureUploaded = false;
        Resource m_skyboxTexture;
        Resource m_skyboxTextureUpload;
        bool m_skyboxTextureUploaded = false;
        bool m_skyboxUploadAttempted = false;
        bool m_skyboxTextureIsHdr = false;
        SkyboxLoadFormat m_skyboxLoadFormat = SkyboxLoadFormat::Auto;
        Resource m_iblIrradianceTexture;
        Resource m_iblIrradianceUpload;
        Resource m_iblPrefilterTexture;
        Resource m_iblPrefilterUpload;
        Resource m_iblBrdfLutTexture;
        Resource m_iblBrdfLutUpload;
        bool m_iblUploaded = false;
        bool m_iblUploadAttempted = false;
        bool m_iblEnabled = false;
        float m_iblIntensity = 0.25f;
        float m_iblPrefilterMaxMip = 0.0f;
        bool m_hdrEquirectLoaded = false;
        bool m_hdrEquirectTried = false;
        UINT m_hdrEquirectWidth = 0;
        UINT m_hdrEquirectHeight = 0;
        std::vector<float> m_hdrEquirectPixels;
        Resource m_skyboxVB;
        VertexBufferView m_skyboxVBV{};
        Viewport m_viewport{};
        Rect m_scissorRect{};
        bool m_comInitialized = false;

        // Cached camera PV matrix (row-major)
        float m_cameraPV[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        float m_cameraPos[3] = { 0.0f, 0.0f, 0.0f };

        Resource m_shadowMap;
        DescriptorHeap m_dsvHeapShadow;
        UINT m_shadowMapSize = 1024;
        Viewport m_shadowViewport{};
        Rect m_shadowScissor{};
        GpuDescriptorHandle m_shadowSrv{};

        float m_lightYaw = 0.7f;
        float m_lightPitch = 1.0f;
        float m_lightDistance = 4.0f;
        float m_lightOrthoHalf = 1.8f;
        float m_lightNear = 0.1f;
        float m_lightFar = 10.0f;
        float m_dirColor[3] = { 1.0f, 1.0f, 1.0f };
        float m_dirIntensity = 1.0f;

        std::vector<PointLight> m_pointLights = { PointLight{} };
        std::vector<SpotLight> m_spotLights;

        Resource m_depth;
        DescriptorHeap m_dsvHeap;

        std::vector<DrawItem> m_drawItems;
        std::unordered_map<uint64_t, Texture*> m_textureCache;

        bool m_useTessellation = false;
        RasterShaderMode m_rasterShaderMode = RasterShaderMode::PBR;
        GBufferDebugView m_gBufferDebugView = GBufferDebugView::FinalLit;
        float m_deltaTime = 0.0f;
        GraphicsRuntime m_graphicsRuntime = GetBuildDefaultGraphicsRuntime();
    };
}
