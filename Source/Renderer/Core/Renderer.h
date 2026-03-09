#pragma once
#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderGraph.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Core/RenderNodeConstants.h"
#include "Renderer/Scene/RenderCameraProxy.h"
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
#include "Renderer/Passes/RenderNodeSetupContext.h"
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
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;
        using RenderNodeType = RendererEnums::RenderNodeType;

        using DirectionalLightSettings = LightSystem::DirectionalLightSettings;
        using PointLight = LightSystem::PointLight;
        using SpotLight = LightSystem::SpotLight;

        using RenderProxy = SasamiRenderer::RenderProxy;

        using OverlayRenderCallback = std::function<void(CommandList&, CpuDescriptorHandle)>;
        using PhaseCompletionMode = RenderGraph::PhaseCompletionMode;
        using PhaseCompletionCallback = std::function<bool(const RenderNodeContextView&)>;

        struct PassHandle
        {
            size_t index = static_cast<size_t>(-1);
            bool IsValid() const { return index != static_cast<size_t>(-1); }
        };

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
        const std::vector<RenderNodeType>& GetRenderNodeSequence() const { return m_renderNodeQueueTypes; }
        void SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence);
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
        std::vector<PointLight>& GetPointLights() { return m_lightSystem.GetPointLights(); }
        const std::vector<PointLight>& GetPointLights() const { return m_lightSystem.GetPointLights(); }
        std::vector<SpotLight>& GetSpotLights() { return m_lightSystem.GetSpotLights(); }
        const std::vector<SpotLight>& GetSpotLights() const { return m_lightSystem.GetSpotLights(); }
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
            Texture* occlusionTexture = nullptr;
            bool transparent = false;
            float model[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        using DrawSceneItemsCallback = std::function<void(bool drawTransparent)>;
        using DrawShadowItemsCallback = std::function<void(const LightSystem::ShadowPassContext&)>;
        struct PhaseCompletionNodeEntry
        {
            std::string phaseTag;
            std::string nodeName;
            PhaseCompletionMode mode = PhaseCompletionMode::Deterministic;
            RenderNodeRequirements requirements{};
            PhaseCompletionCallback execute;
        };

        struct DeferredUploadBatch
        {
            CommandAllocator allocator;
            CommandList commandList;
            std::vector<Resource> uploadResources;
            UINT64 retireFenceValue = 0;
        };

        bool AllocateSrvRange(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu);
        bool InitializeBackBufferTargets(SwapChain& swapChain, UINT bufferCount);
        void ReleaseBackBufferTargets();
        CpuDescriptorHandle GetBackBufferRtv(UINT backIndex) const;
        const Resource* GetBackBufferResource(UINT backIndex) const;
        void RetireDeferredUploadBatches();
        void EnsureEnvironmentTexturesUploaded(CommandList* cmdList);
        PassHandle InsertPassAt(size_t insertIndex, const std::shared_ptr<IRenderNode>& renderPass);
        size_t FindPassIndexByTag(std::string_view targetTag) const;
        bool RegisterPassesToRenderGraph(const RenderGraphExecuteContext& executeContext);
        void RegisterPhaseCompletionNodesToRenderGraph(const RenderGraphExecuteContext& executeContext);
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
        Texture* ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData);
        std::unique_ptr<IRHIDevice> m_device;
        RenderPipelineStateCache m_pipelineStateCache;
        RendererFrameCoordinator m_frameCoordinator;
        std::vector<Mesh> m_meshes;
        MeshBuffer m_meshBuffer;
        DrawCommandBuilder m_drawCommandBuilder;
        std::vector<std::unique_ptr<Texture>> m_sceneTextures;
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

        DescriptorHeap m_srvHeap;
        DescriptorHeap m_rtvHeap;
        UINT m_srvCapacity = 0;
        UINT m_srvNext = 0;
        std::vector<Resource> m_backBuffers;
        std::vector<CpuDescriptorHandle> m_backBufferRtvs;
        GpuDescriptorHandle m_nullTextureSrv{};
        Texture* m_defaultOcclusionTexture = nullptr;
        float m_iblIntensity = 0.25f;
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

        Resource m_depth;
        DescriptorHeap m_dsvHeap;

        std::vector<DrawItem> m_drawItems;
        std::unordered_map<uint64_t, Texture*> m_textureCache;
        std::vector<DeferredUploadBatch> m_deferredUploadBatches;

        bool m_useTessellation = false;
        RasterShaderMode m_rasterShaderMode = RasterShaderMode::Lighting;
        GBufferDebugView m_gBufferDebugView = GBufferDebugView::FinalLit;
        float m_deltaTime = 0.0f;
        GraphicsRuntime m_graphicsRuntime = GetBuildDefaultGraphicsRuntime();
        std::vector<std::shared_ptr<IRenderNode>> m_renderPasses;
        std::vector<PhaseCompletionNodeEntry> m_phaseCompletionNodes;
        std::vector<RenderNodeType> m_renderNodeQueueTypes =
            std::vector<RenderNodeType>(RenderNodeConstants::kDefaultRenderPathSequence.begin(),
                                        RenderNodeConstants::kDefaultRenderPathSequence.end());
    };
}
