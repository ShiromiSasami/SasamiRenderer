#include "Renderer/Core/Renderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <windows.h>
#include <windowsx.h>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "d3dx12.h"

using namespace std;

namespace SasamiRenderer
{
    using Math::Mul4x4;

    Renderer::Renderer()
    {
        ScopedPerfTimer perfTimer("Renderer::Renderer");

        m_shadowRenderNode = std::make_shared<ShadowRenderNode>();
        m_opaqueRenderNode = std::make_shared<OpaqueRenderNode>();
        m_lightingRenderNode = std::make_shared<LightingRenderNode>();
        m_transparentRenderNode = std::make_shared<TransparentRenderNode>();
        m_transparentLightingRenderNode = std::make_shared<TransparentLightingRenderNode>();
        m_skyboxRenderNode = std::make_shared<SkyboxRenderNode>();
        m_postProcessRenderNode = std::make_shared<PostProcessRenderNode>();

        // Rebuild default order through AddPass so runtime order uses one path configuration flow.
        SetRenderNodeSequence(std::vector<RenderNodeType>(RenderNodeConstants::kDefaultRenderPathSequence.begin(),
                                                          RenderNodeConstants::kDefaultRenderPathSequence.end()));
    }

    Renderer::~Renderer()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }

        m_frameCoordinator.Shutdown(m_lightSystem);

        m_skybox.Shutdown();

        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
    }

    bool Renderer::Initialize(HWND hWnd, UINT width, UINT height)
    {
        ScopedPerfTimer perfTimer("Renderer::Initialize");
        auto failInit = [](const char* message) -> bool {
            DebugLogDialog(message, L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        };

        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(coHr)) {
            m_comInitialized = true;
        } else if (coHr != RPC_E_CHANGED_MODE) {
            return failInit("Renderer::Initialize: CoInitializeEx failed.\n");
        }

        RECT rc{};
        GetClientRect(hWnd, &rc);
        UINT clientW = static_cast<UINT>(rc.right - rc.left);
        UINT clientH = static_cast<UINT>(rc.bottom - rc.top);
        if (clientW == 0 || clientH == 0) {
            clientW = width;
            clientH = height;
        }

        if (!IsGraphicsRuntimeEnabled(m_graphicsRuntime)) {
            std::string msg = "Selected graphics runtime is disabled by build symbol: ";
            msg += GraphicsRuntimeToString(m_graphicsRuntime);
            msg += "\n";
            return failInit(msg.c_str());
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.GraphicsDevice");
            m_device = CreateRHIDevice(m_graphicsRuntime);
            if (!m_device) {
                std::string msg = "Failed to create graphics runtime: ";
                msg += GraphicsRuntimeToString(m_graphicsRuntime);
                msg += "\n";
                return failInit(msg.c_str());
            }
            if (!m_device->Initialize(hWnd, clientW, clientH)) {
                return failInit("Renderer::Initialize: IRHIDevice::Initialize failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.BackBufferTargets");
            if (!InitializeBackBufferTargets(m_device->GetSwapChain(), GetBackBufferCount())) {
                return failInit("Renderer::Initialize: back buffer target initialization failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RenderPipelineStateCache");
            if (!m_pipelineStateCache.Initialize(*m_device)) {
                return failInit("Renderer::Initialize: RenderPipelineStateCache::Initialize failed.\n");
            }
        }

        HRESULT hr = S_OK;
        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.SrvHeap");
            D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
            srvHeapDesc.NumDescriptors = 512;
            srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = m_device->CreateDescriptorHeap(srvHeapDesc, m_srvHeap);
            if (FAILED(hr)) {
                return failInit("Renderer::Initialize: SRV descriptor heap creation failed.\n");
            }
            m_srvCapacity = srvHeapDesc.NumDescriptors;
            m_srvNext = 0;
        }

        m_viewport = { 0.0f, 0.0f, static_cast<float>(clientW), static_cast<float>(clientH), 0.0f, 1.0f };
        m_scissorRect = { 0, 0, static_cast<LONG>(clientW), static_cast<LONG>(clientH) };

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.NullTexture");
            CpuDescriptorHandle nullTexCpu{};
            GpuDescriptorHandle nullTexGpu{};
            if (!AllocateSrvRange(1, nullTexCpu, nullTexGpu)) {
                return failInit("Renderer::Initialize: SRV allocation failed for null texture.\n");
            }
            m_nullTextureSrv = nullTexGpu;

            Resource nullResource;
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(nullResource, &nullSrvDesc, nullTexCpu);
        }

        auto allocateSrv = [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) -> bool {
            return AllocateSrvRange(count, outCpu, outGpu);
        };

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.LightSystem");
            if (!m_lightSystem.Initialize(*m_device, allocateSrv)) {
                return failInit("Renderer::Initialize: LightSystem initialization failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.Skybox");
            if (!m_skybox.Initialize(*m_device, allocateSrv)) {
                return failInit("Renderer::Initialize: Skybox initialization failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.FrameCoordinator");
            if (!m_frameCoordinator.Initialize(*m_device,
                                               m_pipelineStateCache,
                                               m_lightSystem,
                                               GetBackBufferCount(),
                                               allocateSrv)) {
                return failInit("Renderer::Initialize: Frame context initialization failed.\n");
            }
        }

        // Create main depth buffer (matches backbuffer size)
        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.MainDepth");
            D3D12_RESOURCE_DESC depthDesc = {};
            depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depthDesc.Width = clientW;
            depthDesc.Height = clientH;
            depthDesc.DepthOrArraySize = 1;
            depthDesc.MipLevels = 1;
            depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
            depthDesc.SampleDesc.Count = 1;
            depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear = {};
            clear.Format = DXGI_FORMAT_D32_FLOAT;
            clear.DepthStencil.Depth = 1.0f;
            clear.DepthStencil.Stencil = 0;

            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            hr = m_device->CreateCommittedResource(&heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &depthDesc,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                   &clear,
                                                   m_depth);
            if (FAILED(hr)) {
                return failInit("Renderer::Initialize: Main depth resource creation failed.\n");
            }

            D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
            dsvDesc.NumDescriptors = 1;
            dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeap);
            if (FAILED(hr)) {
                return failInit("Renderer::Initialize: Main DSV heap creation failed.\n");
            }

            D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            m_device->CreateDepthStencilView(m_depth, &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.DefaultOcclusion");
            DeferredUploadBatch batch;
            HRESULT uploadHr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, batch.allocator);
            if (SUCCEEDED(uploadHr)) {
                uploadHr = m_device->CreateCommandList(0,
                                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       batch.allocator,
                                                       nullptr,
                                                       batch.commandList);
            }
            if (FAILED(uploadHr)) {
                return failInit("Renderer::Initialize: AO fallback upload command list creation failed.\n");
            }

            CpuTextureRgba8 aoFallback;
            aoFallback.pixels = { 255u, 255u, 255u, 255u };
            aoFallback.width = 1;
            aoFallback.height = 1;

            m_defaultOcclusionTexture = CreateTextureFromRgba8Data(aoFallback, &batch.commandList, batch.uploadResources);
            if (!m_defaultOcclusionTexture) {
                return failInit("Renderer::Initialize: AO fallback texture creation failed.\n");
            }

            batch.commandList.Close();
            ID3D12CommandList* uploadLists[] = { batch.commandList.Get() };
            m_device->GetCommandQueue()->ExecuteCommandLists(1, uploadLists);

            batch.retireFenceValue = m_frameCoordinator.SignalQueueFence();
            if (batch.retireFenceValue == 0) {
                m_device->WaitForGPU();
                return failInit("Renderer::Initialize: AO fallback fence signal failed.\n");
            }

            m_deferredUploadBatches.push_back(std::move(batch));
        }

        return true;
    }

    bool Renderer::InitializeBackBufferTargets(SwapChain& swapChain, UINT bufferCount)
    {
        ReleaseBackBufferTargets();

        m_backBuffers.resize(bufferCount);

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = bufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = m_device->CreateDescriptorHeap(rtvHeapDesc, m_rtvHeap);
        if (FAILED(hr)) {
            return false;
        }

        m_backBufferRtvs.resize(bufferCount);
        const UINT rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < bufferCount; ++i) {
            hr = swapChain.GetBuffer(i, IID_PPV_ARGS(m_backBuffers[i].GetAddressOf()));
            if (FAILED(hr)) {
                ReleaseBackBufferTargets();
                return false;
            }

            m_device->CreateRenderTargetView(m_backBuffers[i], nullptr, handle);
            m_backBufferRtvs[i] = handle;
            handle.ptr += rtvDescriptorSize;
        }

        return true;
    }

    void Renderer::ReleaseBackBufferTargets()
    {
        for (auto& backBuffer : m_backBuffers) {
            backBuffer.Reset();
        }
        m_backBuffers.clear();
        m_backBufferRtvs.clear();
        m_rtvHeap.Reset();
    }

    CpuDescriptorHandle Renderer::GetBackBufferRtv(UINT backIndex) const
    {
        if (backIndex >= m_backBufferRtvs.size()) {
            return {};
        }
        return m_backBufferRtvs[backIndex];
    }

    const Resource* Renderer::GetBackBufferResource(UINT backIndex) const
    {
        if (backIndex >= m_backBuffers.size()) {
            return nullptr;
        }
        return &m_backBuffers[backIndex];
    }

    bool Renderer::AllocateSrvRange(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)
    {
        if (!m_device || !m_srvHeap.Get() || count == 0) {
            return false;
        }
        if (m_srvNext + count > m_srvCapacity) {
            DebugLog("SRV heap exhausted\n");
            return false;
        }

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto gpuStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        outCpu.ptr = cpuStart.ptr + static_cast<SIZE_T>(m_srvNext) * inc;
        outGpu.ptr = gpuStart.ptr + static_cast<SIZE_T>(m_srvNext) * inc;
        m_srvNext += count;
        return true;
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

    void Renderer::EnsureEnvironmentTexturesUploaded(CommandList* cmdList)
    {
        if (!m_skybox.IsSkyboxTextureUploaded() && !m_skybox.HasSkyboxUploadAttempted()) {
            m_skybox.EnsureSkyboxTextureUploaded(cmdList);
            return;
        }
        if (!m_skybox.IsIblUploaded() && !m_skybox.HasIblUploadAttempted()) {
            m_skybox.EnsureIblTexturesUploaded(cmdList);
        }
    }

    Renderer::PassHandle Renderer::InsertPassAt(size_t insertIndex, const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!renderPass) {
            return {};
        }

        if (insertIndex > m_renderPasses.size()) {
            insertIndex = m_renderPasses.size();
        }

        m_renderPasses.insert(m_renderPasses.begin() + insertIndex, renderPass);
        return PassHandle{ insertIndex };
    }

    size_t Renderer::FindPassIndexByTag(std::string_view targetTag) const
    {
        if (targetTag.empty()) {
            return static_cast<size_t>(-1);
        }

        for (size_t i = 0; i < m_renderPasses.size(); ++i) {
            const auto& pass = m_renderPasses[i];
            if (!pass) {
                continue;
            }
            if (pass->Tag() == targetTag) {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }

    bool Renderer::RegisterPassesToRenderGraph(const RenderGraphExecuteContext& executeContext)
    {
        std::vector<RenderGraph::PassHandle> orderedPasses;
        orderedPasses.reserve(m_renderPasses.size());

        for (const auto& runtimeNode : m_renderPasses) {
            if (!runtimeNode) {
                DebugLog("Renderer::Render: runtime node is null.\n");
                continue;
            }

            const RenderGraph::PassHandle previousPass =
                orderedPasses.empty() ? RenderGraph::PassHandle{} : orderedPasses.back();
            RenderGraph::PassHandle pass = m_renderGraph.AddPass(*runtimeNode, executeContext, previousPass);
            if (!pass.IsValid()) {
                continue;
            }
            orderedPasses.push_back(pass);
        }

        if (orderedPasses.empty()) {
            DebugLog("Renderer::Render: no executable runtime nodes were registered.\n");
            return false;
        }

        return true;
    }

    void Renderer::RegisterPhaseCompletionNodesToRenderGraph(const RenderGraphExecuteContext& executeContext)
    {
        for (const PhaseCompletionNodeEntry& entry : m_phaseCompletionNodes) {
            if (!entry.execute) {
                DebugLog("Renderer::RegisterPhaseCompletionNodesToRenderGraph: execute callback is null.\n");
                continue;
            }

            const bool registered = m_renderGraph.AddPhaseCompletionNode(
                entry.phaseTag,
                entry.nodeName,
                [entry, executeContext]() -> bool {
                    const RenderNodeContextView contextView = executeContext.CreateContextView(entry.requirements);
                    if (!contextView.IsSatisfied()) {
                        DebugLog("Renderer::RegisterPhaseCompletionNodesToRenderGraph: requirements are not satisfied.\n");
                        return false;
                    }
                    return entry.execute(contextView);
                },
                entry.mode);
            if (!registered) {
                DebugLog("Renderer::RegisterPhaseCompletionNodesToRenderGraph: failed to register completion node.\n");
            }
        }
    }

    RenderNodeExecutionPolicy Renderer::BuildRenderNodeExecutionPolicy(bool executeOpaqueFamilyPasses,
                                                                       bool executeLightingFamilyPasses,
                                                                       bool useShadowTessPath)
    {
        RenderNodeExecutionPolicy policy{};
        policy.executeOpaqueFamilyPasses = executeOpaqueFamilyPasses;
        policy.executeLightingFamilyPasses = executeLightingFamilyPasses;
        policy.useTessellation = m_useTessellation;
        policy.useShadowTessellationPath = useShadowTessPath;
        policy.iblIntensity = m_iblIntensity;
        policy.gBufferDebugView = m_gBufferDebugView;
        return policy;
    }

    RenderNodeFrameInputs Renderer::BuildRenderNodeFrameInputs(CommandList* cmdList,
                                                               RendererFrameCoordinator::FrameContext* frame,
                                                               D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                               GpuDescriptorHandle defaultAoSrv)
    {
        RenderNodeFrameInputs inputs{};
        inputs.cmdList = cmdList;
        inputs.pipelineStateCache = &m_pipelineStateCache;
        inputs.srvHeap = &m_srvHeap;
        inputs.viewport = &m_viewport;
        inputs.scissorRect = &m_scissorRect;
        inputs.frameCoordinator = &m_frameCoordinator;
        inputs.frame = frame;

        inputs.lightSystem = &m_lightSystem;
        inputs.frameLight = frame ? &frame->light : nullptr;
        inputs.skybox = &m_skybox;
        inputs.cameraPV = m_cameraPV;
        inputs.cameraPos = m_cameraPos;
        inputs.shadowSrv = m_lightSystem.GetShadowSrv();
        inputs.lightSrvTable = frame ? frame->light.lightSrvTable : GpuDescriptorHandle{};
        inputs.iblSrvTable = m_skybox.GetIblSrvTable();
        inputs.aoSrv = defaultAoSrv;
        inputs.lightCbGpu = lightCbGpu;
        return inputs;
    }

    RenderNodeExecutionServices Renderer::BuildRenderNodeExecutionServices(
        const DrawSceneItemsCallback& drawItems,
        const DrawShadowItemsCallback& drawShadowItems)
    {
        RenderNodeExecutionServices services{};
        services.drawOpaqueItems = [drawItems]() { drawItems(false); };
        services.drawTransparentItems = [drawItems]() { drawItems(true); };
        services.drawShadowItems = drawShadowItems;
        return services;
    }

    void Renderer::Render(const OverlayRenderCallback& overlay)
    {
        if (!m_device) {
            return;
        }

        RetireDeferredUploadBatches();

        const UINT backIndex = m_device->GetSwapChain()->GetCurrentBackBufferIndex();
        auto* frame = m_frameCoordinator.GetFrameContext(backIndex);
        if (!frame) {
            return;
        }

        CommandList* cmdList = nullptr;
        if (!m_frameCoordinator.BeginFrame(backIndex, cmdList)) {
            return;
        }
        m_frameCoordinator.EnsureCameraBuffers(*frame, static_cast<UINT>(m_drawItems.size() * 2u + 4u));

        EnsureEnvironmentTexturesUploaded(cmdList);

        m_renderGraph.Clear();
        const auto* backBuffer = GetBackBufferResource(backIndex);
        if (!backBuffer || !m_depth.IsValid() || !m_dsvHeap.Get()) {
            return;
        }

        ExternalRenderGraphResourceDesc sceneColorDesc{};
        sceneColorDesc.resource = backBuffer->Get();
        sceneColorDesc.initialState = D3D12_RESOURCE_STATE_PRESENT;
        sceneColorDesc.finalState = D3D12_RESOURCE_STATE_PRESENT;
        sceneColorDesc.transitionToFinalState = true;
        sceneColorDesc.rtv = GetBackBufferRtv(backIndex);
        sceneColorDesc.hasRtv = true;
        sceneColorDesc.clearColorOnFirstUse = true;
        sceneColorDesc.clearColor[0] = 0.2f;
        sceneColorDesc.clearColor[1] = 0.2f;
        sceneColorDesc.clearColor[2] = 0.2f;
        sceneColorDesc.clearColor[3] = 1.0f;
        m_renderGraph.ImportExternalResource("SceneColor", sceneColorDesc);

        ExternalRenderGraphResourceDesc sceneDepthDesc{};
        sceneDepthDesc.resource = m_depth.Get();
        sceneDepthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        sceneDepthDesc.finalState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        sceneDepthDesc.transitionToFinalState = true;
        sceneDepthDesc.dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        sceneDepthDesc.hasDsv = true;
        sceneDepthDesc.clearDepthOnFirstUse = true;
        sceneDepthDesc.clearDepth = 1.0f;
        sceneDepthDesc.clearStencil = 0;
        m_renderGraph.ImportExternalResource("SceneDepth", sceneDepthDesc);

        auto drawItems = [this, cmdList, frame](bool drawTransparent) {
            for (const auto& item : m_drawItems) {
                if (item.transparent != drawTransparent) {
                    continue;
                }

                float objMVP[16];
                Mul4x4(item.model, m_cameraPV, objMVP);
                const D3D12_GPU_VIRTUAL_ADDRESS cameraCbGpu =
                    m_frameCoordinator.PushCameraCB(*frame, objMVP, item.model);
                if (cameraCbGpu != 0) {
                    cmdList->SetGraphicsRootConstantBufferView(2, cameraCbGpu);
                }

                if (item.texture) {
                    cmdList->SetGraphicsRootDescriptorTable(0, item.texture->srv);
                } else {
                    cmdList->SetGraphicsRootDescriptorTable(0, m_nullTextureSrv);
                }

                if (item.occlusionTexture) {
                    cmdList->SetGraphicsRootDescriptorTable(6, item.occlusionTexture->srv);
                } else if (m_defaultOcclusionTexture) {
                    cmdList->SetGraphicsRootDescriptorTable(6, m_defaultOcclusionTexture->srv);
                } else {
                    cmdList->SetGraphicsRootDescriptorTable(6, m_nullTextureSrv);
                }

                m_meshBuffer.Bind(cmdList, item.meshIndex);
                const auto& items = m_meshBuffer.Items();
                if (item.meshIndex < items.size()) {
                    const auto& it = items[item.meshIndex];
                    if (it.indexCount > 0) {
                        cmdList->DrawIndexedInstanced(it.indexCount, 1, 0, 0, 0);
                    } else if (it.vertexCount > 0) {
                        cmdList->DrawInstanced(it.vertexCount, 1, 0, 0);
                    }
                }
            }
        };

        const D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu = frame->light.lightCB.IsValid()
            ? frame->light.lightCB->GetGPUVirtualAddress()
            : 0;
        const GpuDescriptorHandle defaultAoSrv =
            (m_defaultOcclusionTexture != nullptr) ? m_defaultOcclusionTexture->srv : m_nullTextureSrv;

        const bool executeOpaqueFamilyPasses = (m_rasterShaderMode == RasterShaderMode::Opaque);
        const bool executeLightingFamilyPasses = !executeOpaqueFamilyPasses;
        const bool hasLightingPass =
            executeLightingFamilyPasses &&
            std::any_of(m_renderPasses.begin(),
                        m_renderPasses.end(),
                        [](const std::shared_ptr<IRenderNode>& runtimeNode) {
                            if (!runtimeNode) {
                                return false;
                            }
                            const std::string_view tag = runtimeNode->Tag();
                            return tag == "Lighting" || tag == "TransparentLighting";
                        });
        const bool useShadowTessPath = hasLightingPass && m_useTessellation;

        auto drawShadowItems = [this, cmdList, frame](const LightSystem::ShadowPassContext& context) {
            for (const auto& item : m_drawItems) {
                float objLightMVP[16];
                Mul4x4(item.model, context.lightViewProjection, objLightMVP);
                const D3D12_GPU_VIRTUAL_ADDRESS cameraCbGpu =
                    m_frameCoordinator.PushCameraCB(*frame, objLightMVP, item.model);
                if (cameraCbGpu != 0) {
                    cmdList->SetGraphicsRootConstantBufferView(2, cameraCbGpu);
                }

                m_meshBuffer.Bind(cmdList, item.meshIndex);
                const auto& items = m_meshBuffer.Items();
                if (item.meshIndex < items.size()) {
                    const auto& it = items[item.meshIndex];
                    if (it.indexCount > 0) {
                        cmdList->DrawIndexedInstanced(it.indexCount, 1, 0, 0, 0);
                    } else if (it.vertexCount > 0) {
                        cmdList->DrawInstanced(it.vertexCount, 1, 0, 0);
                    }
                }
            }
        };

        const RenderNodeExecutionPolicy executionPolicy = BuildRenderNodeExecutionPolicy(
            executeOpaqueFamilyPasses,
            executeLightingFamilyPasses,
            useShadowTessPath);
        const RenderNodeFrameInputs frameInputs = BuildRenderNodeFrameInputs(
            cmdList,
            frame,
            lightCbGpu,
            defaultAoSrv);
        const RenderNodeExecutionServices executionServices = BuildRenderNodeExecutionServices(
            drawItems,
            drawShadowItems);

        RenderGraphExecuteContext executeContext{};
        executeContext.executionPolicy = &executionPolicy;
        executeContext.frameInputs = &frameInputs;
        executeContext.executionServices = &executionServices;
        executeContext.resources = &m_renderGraph.GetResourceRegistry();

        if (!RegisterPassesToRenderGraph(executeContext)) {
            m_renderGraph.Clear();
            if (overlay) {
                TransitionBackBufferToRenderTarget(cmdList, backIndex);
                ClearAndBindMainTargets(cmdList, backIndex);
                overlay(*cmdList, GetBackBufferRtv(backIndex));
                TransitionBackBufferToPresent(cmdList, backIndex);
            }
            SubmitAndPresent(cmdList, backIndex);
            return;
        }

        RegisterPhaseCompletionNodesToRenderGraph(executeContext);

        const bool graphExecuted = m_renderGraph.Execute();
        m_renderGraph.Clear();
        if (!graphExecuted) {
            if (overlay) {
                TransitionBackBufferToRenderTarget(cmdList, backIndex);
                BindMainTargets(cmdList, backIndex);
                overlay(*cmdList, GetBackBufferRtv(backIndex));
                TransitionBackBufferToPresent(cmdList, backIndex);
            }
            SubmitAndPresent(cmdList, backIndex);
            return;
        }

        if (overlay) {
            TransitionBackBufferToRenderTarget(cmdList, backIndex);
            BindMainTargets(cmdList, backIndex);
            overlay(*cmdList, GetBackBufferRtv(backIndex));
            TransitionBackBufferToPresent(cmdList, backIndex);
        }
        SubmitAndPresent(cmdList, backIndex);
    }
    Renderer::DirectionalLightSettings Renderer::GetDirectionalLightSettings() const
    {
        return m_lightSystem.GetDirectionalLightSettings();
    }

    void Renderer::SetDirectionalLightSettings(const DirectionalLightSettings& settings)
    {
        m_lightSystem.SetDirectionalLightSettings(settings);
    }

    void Renderer::ResizeViewport(UINT width, UINT height)
    {
        if (width == 0 || height == 0 || !m_device) {
            return;
        }

        m_device->WaitForGPU();
        m_frameCoordinator.ResetFrameFenceValues();

        ReleaseBackBufferTargets();

        HRESULT hr = m_device->GetSwapChain()->ResizeBuffers(GetBackBufferCount(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(hr)) {
            DebugLog("ResizeBuffers failed\n");
            return;
        }

        if (!InitializeBackBufferTargets(m_device->GetSwapChain(), GetBackBufferCount())) {
            DebugLog("Back buffer target rebuild failed\n");
            return;
        }

        m_viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

        m_depth.Reset();
        m_dsvHeap.Reset();

        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr2 = m_device->CreateCommittedResource(&heap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &depthDesc,
                                                        D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                        &clear,
                                                        m_depth);
        if (FAILED(hr2)) {
            DebugLog("Depth recreation failed\n");
            return;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr2 = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeap);
        if (FAILED(hr2)) {
            DebugLog("DSV heap recreation failed\n");
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        m_device->CreateDepthStencilView(m_depth, &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    void Renderer::RefreshEnvironmentAssets()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
        m_skybox.RefreshEnvironmentAssets();
    }

    void Renderer::SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height)
    {
        m_skybox.SetHdrEquirectData(std::move(pixels), width, height);
    }

    void Renderer::SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height)
    {
        m_skybox.SetLdrEquirectData(std::move(pixels), width, height);
    }

    void Renderer::SetSkyboxLoadFormat(SkyboxLoadFormat format)
    {
        m_skybox.SetLoadFormat(format);
    }

    Renderer::PassHandle Renderer::AddPass(const std::shared_ptr<IRenderNode>& renderPass)
    {
        return InsertPassAt(m_renderPasses.size(), renderPass);
    }

    Renderer::PassHandle Renderer::AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return {};
        }
        return InsertPassAt(targetIndex, renderPass);
    }

    Renderer::PassHandle Renderer::AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return {};
        }
        return InsertPassAt(targetIndex + 1u, renderPass);
    }

    bool Renderer::ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!renderPass) {
            return false;
        }
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return false;
        }
        m_renderPasses[targetIndex] = renderPass;
        return true;
    }

    bool Renderer::AddPhaseCompletionNode(std::string_view phaseTag,
                                          std::string_view nodeName,
                                          const PhaseCompletionCallback& execute,
                                          PhaseCompletionMode mode,
                                          const RenderNodeRequirements& requirements)
    {
        if (phaseTag.empty() || !execute) {
            return false;
        }

        PhaseCompletionNodeEntry entry{};
        entry.phaseTag.assign(phaseTag.begin(), phaseTag.end());
        entry.nodeName.assign(nodeName.begin(), nodeName.end());
        entry.mode = mode;
        entry.requirements = requirements;
        entry.execute = execute;
        m_phaseCompletionNodes.push_back(std::move(entry));
        return true;
    }

    void Renderer::ClearPhaseCompletionNodes()
    {
        m_phaseCompletionNodes.clear();
    }

    void Renderer::ClearPasses()
    {
        m_renderPasses.clear();
        m_renderNodeQueueTypes.clear();
    }

    void Renderer::SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence)
    {
        if (sequence.empty()) {
            DebugLog("Renderer::SetRenderNodeSequence: empty sequence is ignored.\n");
            return;
        }

        ClearPasses();
        m_renderNodeQueueTypes.clear();
        const std::array<std::shared_ptr<IRenderNode>, static_cast<size_t>(RenderNodeType::PostProcess) + 1u> builtinNodes = {
            m_shadowRenderNode,
            m_opaqueRenderNode,
            m_lightingRenderNode,
            m_transparentRenderNode,
            m_transparentLightingRenderNode,
            m_skyboxRenderNode,
            m_postProcessRenderNode
        };

        for (const RenderNodeType nodeType : sequence) {
            const size_t nodeIndex = static_cast<size_t>(nodeType);
            if (nodeIndex >= builtinNodes.size() || builtinNodes[nodeIndex] == nullptr) {
                DebugLog("Renderer::SetRenderNodeSequence: invalid node type is ignored.\n");
                continue;
            }
            AddPass(builtinNodes[nodeIndex]);
            m_renderNodeQueueTypes.push_back(nodeType);
        }
    }

    void Renderer::TransitionBackBufferToRenderTarget(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = GetBackBufferResource(backIndex);
        barrier.Transition.pResource = backBuffer ? backBuffer->Get() : nullptr;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void Renderer::BindMainTargets(CommandList* cmdList, UINT backIndex)
    {
        auto rtvHandle = GetBackBufferRtv(backIndex);
        auto dsvMain = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvMain);
    }

    void Renderer::ClearAndBindMainTargets(CommandList* cmdList, UINT backIndex)
    {
        BindMainTargets(cmdList, backIndex);
        auto rtvHandle = GetBackBufferRtv(backIndex);
        auto dsvMain = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        const FLOAT clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(dsvMain, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    Texture* Renderer::CreateTextureFromRgba8Data(const CpuTextureRgba8& src,
                                                  CommandList* cmdList,
                                                  std::vector<Resource>& uploads)
    {
        if (src.pixels.empty() || src.width == 0 || src.height == 0 || !cmdList) {
            return nullptr;
        }

        Resource texture;
        Resource upload;
        if (!ResourceUploadUtility::CreateTexture2DFromRgba8(*m_device,
                                                             cmdList,
                                                             src.pixels.data(),
                                                             src.width,
                                                             src.height,
                                                             texture,
                                                             upload)) {
            return nullptr;
        }

        CpuDescriptorHandle cpu{};
        GpuDescriptorHandle gpu{};
        if (!AllocateSrvRange(1, cpu, gpu)) {
            return nullptr;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(texture, &srvDesc, cpu);

        auto texObj = std::make_unique<Texture>();
        texObj->resource = texture;
        texObj->srv = gpu;
        texObj->desc.width = src.width;
        texObj->desc.height = src.height;
        texObj->desc.mips = 1;
        texObj->desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;

        uploads.push_back(upload);
        m_sceneTextures.push_back(std::move(texObj));
        return m_sceneTextures.back().get();
    }

    void Renderer::TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = GetBackBufferResource(backIndex);
        barrier.Transition.pResource = backBuffer ? backBuffer->Get() : nullptr;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void Renderer::SubmitAndPresent(CommandList* cmdList, UINT frameIndex)
    {
        if (FAILED(cmdList->Close())) {
            DebugLog("Failed to close command list\n");
            return;
        }

        ID3D12CommandList* lists[] = { cmdList->Get() };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, lists);
        (void)m_device->GetSwapChain()->Present(1, 0);
        m_frameCoordinator.SignalFrame(frameIndex);
        RetireDeferredUploadBatches();
    }

    void Renderer::UpdateCameraCB(const RenderCameraProxy* camera)
    {
        if (camera) {
            for (int i = 0; i < 16; ++i) {
                m_cameraPV[i] = camera->viewProjection[i];
            }
            m_cameraPos[0] = camera->cameraPosition[0];
            m_cameraPos[1] = camera->cameraPosition[1];
            m_cameraPos[2] = camera->cameraPosition[2];
        } else {
            for (int i = 0; i < 16; ++i) {
                m_cameraPV[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            }
            m_cameraPos[0] = 0.0f;
            m_cameraPos[1] = 0.0f;
            m_cameraPos[2] = 0.0f;
        }
    }

    Texture* Renderer::ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData)
    {
        if (!textureData) {
            return nullptr;
        }

        const uint64_t textureId = textureData->id;
        if (textureId == 0) {
            return nullptr;
        }

        auto cached = m_textureCache.find(textureId);
        if (cached != m_textureCache.end()) {
            return cached->second;
        }

        CommandAllocator uploadAlloc;
        CommandList uploadList;
        HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc);
        if (SUCCEEDED(hr)) {
            hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc, nullptr, uploadList);
        }
        if (FAILED(hr)) {
            return nullptr;
        }

        std::vector<Resource> uploads;
        Texture* texture = CreateTextureFromRgba8Data(*textureData, &uploadList, uploads);
        if (!texture) {
            return nullptr;
        }

        uploadList->Close();
        ID3D12CommandList* lists[] = { uploadList.Get() };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, lists);
        m_device->WaitForGPU();
        uploads.clear();

        m_textureCache[textureId] = texture;
        return texture;
    }

    void Renderer::SubmitRenderProxies(std::vector<RenderProxy>&& proxies)
    {
        if (!m_device || proxies.empty()) {
            return;
        }

        for (auto& proxy : proxies) {
            const size_t meshIndex = m_meshes.size();
            m_meshes.push_back(std::move(proxy.mesh));

            DrawItem item;
            item.meshIndex = meshIndex;
            item.texture = ResolveSceneTexture(proxy.albedoTexture);
            if (!item.texture) {
                if (proxy.albedoTexture) {
                    DebugLog("Renderer::SubmitRenderProxies: failed to resolve albedo texture. Null SRV is bound.\n");
                } else {
                    DebugLog("Renderer::SubmitRenderProxies: missing albedo texture. Null SRV is bound.\n");
                }
            }
            item.occlusionTexture = ResolveSceneTexture(proxy.occlusionTexture);
            item.transparent = proxy.transparent;
            if (!item.occlusionTexture && proxy.occlusionTexture) {
                DebugLog("Renderer::SubmitRenderProxies: failed to resolve occlusion texture. AO fallback is bound.\n");
            }

            std::memcpy(item.model, proxy.model, sizeof(item.model));
            m_drawItems.push_back(item);
        }

        m_meshBuffer.Upload(*m_device, m_meshes);
    }

    void Renderer::ClearSubmittedRenderProxies()
    {
        m_drawItems.clear();
        m_meshes.clear();
    }

    void Renderer::ClearRenderObjects()
    {
        ClearSubmittedRenderProxies();
    }
}
