#pragma once
#include "GraphicsDevice.h"
#include <vector>
#include "RenderTargetResourceBinder.h"
#include "RenderLayerConfigurator.h"
#include "RenderCameraProxy.h"
#include "RenderProxy.h"
#include "MeshBuffer.h"
#include "DrawCommandBuilder.h"
#include "Material.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace SasamiRenderer
{
    class Renderer
    {
    public:
        struct DirectionalLightSettings
        {
            float yaw = 0.7f;
            float pitch = 1.0f;
            float distance = 4.0f;
            float orthoHalf = 1.8f;
            float nearZ = 0.1f;
            float farZ = 10.0f;
            float color[3] = { 1.0f, 1.0f, 1.0f };
            float intensity = 1.0f;
        };

        struct PointLight
        {
            float pos[3] = { 0.0f, 1.5f, 0.0f };
            float range = 5.0f;
            float color[3] = { 1.0f, 1.0f, 1.0f };
            float intensity = 1.0f;
        };

        struct SpotLight
        {
            float pos[3] = { 0.0f, 2.0f, -2.0f };
            float range = 6.0f;
            float yaw = 0.0f;
            float pitch = -0.6f;
            float color[3] = { 1.0f, 1.0f, 1.0f };
            float intensity = 1.0f;
            float innerAngle = 0.261799f; // 15 deg
            float outerAngle = 0.436332f; // 25 deg
        };

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
        void SetGraphicsRuntime(GraphicsRuntime runtime) { m_graphicsRuntime = runtime; }
        GraphicsRuntime GetGraphicsRuntime() const { return m_graphicsRuntime; }
        void SetRHIBackend(RHIBackend backend) { SetGraphicsRuntime(backend); }
        RHIBackend GetRHIBackend() const { return GetGraphicsRuntime(); }
        void SetGraphicsBackend(GraphicsBackend backend) { SetRHIBackend(backend); }
        GraphicsBackend GetGraphicsBackend() const { return GetRHIBackend(); }

        void SetDirectionalLightAngles(float yaw, float pitch);
        void SetDirectionalLightDistance(float distance);
        void SetDirectionalLightOrtho(float halfExtent, float nearZ, float farZ);
        void ResizeViewport(UINT width, UINT height);
        bool GetUseTessellation() const { return m_useTessellation; }
        void SetUseTessellation(bool enable) { m_useTessellation = enable; }
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
            void* cameraCBPtr = nullptr;
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
        void MainPass(CommandList* cmdList, FrameContext& frame);
        void TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex);
        void SubmitAndPresent(CommandList* cmdList, UINT frameIndex);
        Texture* CreateTextureFromFile(const std::wstring& path, CommandList* cmdList,
                                       std::vector<Resource>& uploads);
        Texture* ResolveSceneTexture(const std::wstring& path);
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

        Resource m_texture;
        Resource m_textureUpload;
        bool m_textureUploaded = false;
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
        std::unordered_map<std::wstring, Texture*> m_textureCache;

        bool m_useTessellation = false;
        float m_deltaTime = 0.0f;
        GraphicsRuntime m_graphicsRuntime = GetBuildDefaultGraphicsRuntime();
    };
}
