#include "Renderer/Core/Renderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
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
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "d3dx12.h"

using namespace std;

namespace SasamiRenderer
{
    using Math::Mul4x4;

    namespace
    {
        uint32_t ComputeScaledDimension(uint32_t dimension, float scale)
        {
            return std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(dimension) * scale)));
        }

        bool UsesSsaoAmbientOcclusionMode(RendererEnums::AmbientOcclusionMode mode)
        {
            return mode == RendererEnums::AmbientOcclusionMode::SSAOOnly ||
                   mode == RendererEnums::AmbientOcclusionMode::Hybrid;
        }

        bool UsesSwrtAmbientOcclusionMode(RendererEnums::AmbientOcclusionMode mode)
        {
            return mode == RendererEnums::AmbientOcclusionMode::SWRTAOOnly;
        }
    }

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
        m_rayTracingRenderNode = std::make_shared<RayTracingRenderNode>();
        m_ssaoRenderNode = std::make_shared<SSAORenderNode>();
        m_proceduralSkyRenderNode = std::make_shared<ProceduralSkyRenderNode>();
        m_sdfFluidRenderNode = std::make_shared<SdfFluidRenderNode>();
        m_volumetricCloudRenderNode = std::make_shared<VolumetricCloudRenderNode>();
        m_debugProbeGridRenderNode = std::make_shared<DebugProbeGridRenderNode>();

        m_passRegistry.SetBuiltinNodes({
            m_shadowRenderNode, m_opaqueRenderNode, m_lightingRenderNode,
            m_transparentRenderNode, m_transparentLightingRenderNode,
            m_skyboxRenderNode, m_postProcessRenderNode, m_ssaoRenderNode,
            m_proceduralSkyRenderNode, m_sdfFluidRenderNode
        });

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
            if (!m_renderTargetPool.InitializeBackBuffers(*m_device, m_device->GetSwapChain(), GetBackBufferCount())) {
                return failInit("Renderer::Initialize: back buffer target initialization failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RenderPipelineStateCache");
            if (!m_pipelineStateCache.Initialize(*m_device)) {
                return failInit("Renderer::Initialize: RenderPipelineStateCache::Initialize failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.SrvHeap");
            if (!m_srvAllocator.Initialize(*m_device, 512)) {
                return failInit("Renderer::Initialize: SRV descriptor heap creation failed.\n");
            }
        }

        m_viewport = { 0.0f, 0.0f, static_cast<float>(clientW), static_cast<float>(clientH), 0.0f, 1.0f };
        m_scissorRect = { 0, 0, static_cast<LONG>(clientW), static_cast<LONG>(clientH) };

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.NullTexture");
            CpuDescriptorHandle nullTexCpu{};
            GpuDescriptorHandle nullTexGpu{};
            if (!m_srvAllocator.Allocate(1, nullTexCpu, nullTexGpu)) {
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

        // (SWRT/ReSTIR SRV slots and descriptor heaps are now initialized by RenderTargetPool)

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracingDescriptors");
            CpuDescriptorHandle rtCpu{};
            GpuDescriptorHandle rtGpu{};
            if (!m_srvAllocator.Allocate(6, rtCpu, rtGpu)) {
                return failInit("Renderer::Initialize: ray tracing descriptor reservation failed.\n");
            }

            const UINT descriptorSize =
                m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const UINT baseIndex = m_srvAllocator.GetIndex(rtGpu);

            m_rayTracingDescriptors.outputUavCpu = rtCpu;
            m_rayTracingDescriptors.outputDescriptorIndex = baseIndex;

            m_rayTracingDescriptors.tlasSrvCpu = { rtCpu.ptr + descriptorSize * 1u };
            m_rayTracingDescriptors.tlasDescriptorIndex = baseIndex + 1u;

            m_rayTracingDescriptors.vertexSrvCpu = { rtCpu.ptr + descriptorSize * 2u };
            m_rayTracingDescriptors.vertexDescriptorIndex = baseIndex + 2u;

            m_rayTracingDescriptors.indexSrvCpu = { rtCpu.ptr + descriptorSize * 3u };
            m_rayTracingDescriptors.indexDescriptorIndex = baseIndex + 3u;

            m_rayTracingDescriptors.materialSrvCpu = { rtCpu.ptr + descriptorSize * 4u };
            m_rayTracingDescriptors.materialDescriptorIndex = baseIndex + 4u;

            m_rayTracingDescriptors.instanceSrvCpu = { rtCpu.ptr + descriptorSize * 5u };
            m_rayTracingDescriptors.instanceDescriptorIndex = baseIndex + 5u;
        }

        auto allocateSrv = [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) -> bool {
            return m_srvAllocator.Allocate(count, outCpu, outGpu);
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

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RenderTargetPool");
            if (!m_renderTargetPool.Initialize(*m_device, clientW, clientH, GetBackBufferCount(), allocateSrv)) {
                return failInit("Renderer::Initialize: RenderTargetPool initialization failed.\n");
            }
            if (!m_renderTargetPool.EnsureSSAO(*m_device, clientW, clientH)) {
                return failInit("Renderer::Initialize: SSAO resource creation failed.\n");
            }
            if (!m_renderTargetPool.EnsureGBuffer(*m_device, clientW, clientH)) {
                return failInit("Renderer::Initialize: GBuffer resource creation failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.DefaultMaterialTextures");
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
                return failInit("Renderer::Initialize: default material texture upload command list creation failed.\n");
            }

            CpuTextureRgba8 albedoFallback;
            albedoFallback.pixels = { 255u, 255u, 255u, 255u };
            albedoFallback.width = 1;
            albedoFallback.height = 1;
            m_defaultAlbedoTexture = CreateTextureFromRgba8Data(albedoFallback, &batch.commandList, batch.uploadResources);
            if (!m_defaultAlbedoTexture) {
                return failInit("Renderer::Initialize: albedo fallback texture creation failed.\n");
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

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing");
            if (!m_renderTargetPool.EnsureRayTracingOutput(*m_device, clientW, clientH)) {
                return failInit("Renderer::Initialize: ray tracing resources initialization failed.\n");
            }
            m_rayTracingStats.hardwareSupported = IsHardwareRayTracingSupported();
            if (!m_gpuSoftwareRayTracer.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: GpuSoftwareRayTracer initialization failed.\n");
            }
            if (!m_probeGrid.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: IrradianceProbeGrid initialization failed.\n");
            }
            if (m_debugProbeGridRenderNode) {
                if (!m_debugProbeGridRenderNode->Initialize(*m_device)) {
                    DebugLog("Renderer::Initialize: DebugProbeGridRenderNode initialization failed (non-fatal).\n");
                } else {
                    m_debugProbeGridRenderNode->SetProbeGrid(&m_probeGrid);
                }
            }
            if (m_rayTracingStats.hardwareSupported) {
                if (!m_dxrRayTracer.Initialize(*m_device, m_rayTracingDescriptors)) {
                    DebugLog("Renderer::Initialize: DXR initialization failed. Hardware RT is unavailable.\n");
                }
            }
        }

        {
            SWRTExecutor::InitParams swrtParams{};
            swrtParams.device               = m_device.get();
            swrtParams.renderTargetPool     = &m_renderTargetPool;
            swrtParams.gpuSoftwareRayTracer = &m_gpuSoftwareRayTracer;
            swrtParams.dxrRayTracer         = &m_dxrRayTracer;
            swrtParams.rayTracingScene      = &m_rayTracingScene;
            swrtParams.lightSystem          = &m_lightSystem;
            swrtParams.skybox               = &m_skybox;
            swrtParams.probeGrid            = &m_probeGrid;
            swrtParams.srvHeap              = m_srvAllocator.GetHeap();
            m_swrtExecutor.Initialize(swrtParams);
        }

        {
            SceneSubmitter::InitParams submitterParams{};
            submitterParams.device          = m_device.get();
            submitterParams.meshBuffer      = &m_meshBuffer;
            submitterParams.rayTracingScene = &m_rayTracingScene;
            submitterParams.dxrRayTracer    = &m_dxrRayTracer;
            submitterParams.srvAllocFn      = [this](UINT count, CpuDescriptorHandle& cpu, GpuDescriptorHandle& gpu) {
                return m_srvAllocator.Allocate(count, cpu, gpu);
            };
            submitterParams.srvIndexFn      = [this](GpuDescriptorHandle handle) {
                return m_srvAllocator.GetIndex(handle);
            };
            m_sceneSubmitter.Initialize(submitterParams);
        }

        // ---- Async compute resources ----
        // Non-fatal: if compute queue is unavailable, SWRT falls back to graphics queue.
        if (m_device->GetComputeQueue().IsValid()) {
            const UINT frameCount = GetBackBufferCount();
            m_computeAllocators.resize(frameCount);
            bool computeOk = true;
            for (UINT i = 0; i < frameCount && computeOk; ++i) {
                if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                            m_computeAllocators[i]))) {
                    DebugLog("Renderer::Initialize: compute command allocator creation failed.\n");
                    computeOk = false;
                }
            }
            if (computeOk) {
                if (FAILED(m_device->CreateCommandList(0,
                                                       D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                       m_computeAllocators[0],
                                                       nullptr,
                                                       m_computeCmdList))) {
                    DebugLog("Renderer::Initialize: compute command list creation failed.\n");
                    computeOk = false;
                }
            }
            if (computeOk) {
                m_computeCmdList.Close();
                m_computeCmdListReady = true;
                if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                 m_crossQueueFence.GetAddressOf()))) {
                    DebugLog("Renderer::Initialize: cross-queue fence creation failed.\n");
                    m_computeCmdListReady = false;
                } else {
                    DebugLog("Renderer::Initialize: async compute queue initialized.\n");
                }
            }
            if (!computeOk) {
                m_computeAllocators.clear();
            }
        }

        m_passRegistry.SetBuiltinNodes({
            m_shadowRenderNode, m_opaqueRenderNode, m_lightingRenderNode,
            m_transparentRenderNode, m_transparentLightingRenderNode,
            m_skyboxRenderNode, m_postProcessRenderNode, m_ssaoRenderNode,
            m_proceduralSkyRenderNode, m_sdfFluidRenderNode
        });

        // Propagate cloud settings to SdfFluidRenderNode (RayMarch sky/cloud uses these too).
        if (m_sdfFluidRenderNode) {
            m_sdfFluidRenderNode->SetCloudCover(m_settings.cloudCover);
            m_sdfFluidRenderNode->SetCloudDensity(m_settings.cloudDensity);
        }

        // PBR no longer inserts the volumetric cloud pass. RayMarch sky/cloud stays on the
        // SdfFluid path, so cloud settings are still forwarded there.

        // Insert the debug probe grid after sky composition so deferred lighting has already
        // produced SceneColor. Transparent passes still come later and can draw over it.
        if (m_debugProbeGridRenderNode && m_debugProbeGridRenderNode->IsInitialized()) {
            if (!AddPassAfter("Skybox", m_debugProbeGridRenderNode).IsValid()) {
                if (!AddPassAfter("Lighting", m_debugProbeGridRenderNode).IsValid()) {
                    AddPass(m_debugProbeGridRenderNode);
                }
            }
        }

        return true;
    }

    // Back buffer management is now delegated to RenderTargetPool.

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

    SWRTExecutor::FrameContext Renderer::BuildSwrtFrameContext() const
    {
        SWRTExecutor::FrameContext ctx{};
        std::memcpy(ctx.cameraPos,   m_cameraState.GetPos(),   sizeof(ctx.cameraPos));
        std::memcpy(ctx.cameraInvPV, m_cameraState.GetInvPV(), sizeof(ctx.cameraInvPV));
        ctx.viewportWidth  = m_viewport.Width;
        ctx.viewportHeight = m_viewport.Height;
        ctx.deltaTime      = m_deltaTime;
        return ctx;
    }

    RenderNodeExecutionPolicy Renderer::BuildRenderNodeExecutionPolicy(bool executeOpaqueFamilyPasses,
                                                                       bool executeLightingFamilyPasses,
                                                                       bool useShadowTessPath)
    {
        const SWRTExecutor::PartialBehavior partialBehavior =
            m_swrtExecutor.ResolveBehavior(m_settings.rayTracingPerformancePreset);
        RenderNodeExecutionPolicy policy{};
        policy.executeOpaqueFamilyPasses = executeOpaqueFamilyPasses;
        policy.executeLightingFamilyPasses = executeLightingFamilyPasses;
        policy.useTessellation = m_settings.useTessellation;
        policy.useTessellationWireframe = m_settings.tessWireframeEnabled;
        policy.useTessellationDebugColors = m_settings.tessDebugColorsEnabled;
        policy.useMeshletDebugView = m_settings.meshletDebugViewEnabled;
        policy.useShadowTessellationPath = useShadowTessPath;
        policy.useSoftwareRayTracedDirectionalShadow =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled &&
            m_renderTargetPool.GetSWRTShadowTexture().IsValid();
        policy.useSoftwareRayTracedReflections =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled &&
            m_renderTargetPool.GetSWRTReflectionTexture().IsValid();
        policy.softwareRayTracedShadowMapSize =
            policy.useSoftwareRayTracedDirectionalShadow
                ? partialBehavior.shadowMapSize
                : 1024u;
        policy.renderWidth = static_cast<uint32_t>(std::max(0.0f, m_viewport.Width));
        policy.renderHeight = static_cast<uint32_t>(std::max(0.0f, m_viewport.Height));
        policy.iblIntensity = m_settings.iblIntensity;
        policy.gBufferDebugView = m_settings.gBufferDebugView;
        policy.renderPathMode = m_settings.renderPathMode;
        m_lightSystem.SetAoMinOcclusion(m_settings.aoMinOcclusion);
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
        inputs.srvHeap = m_srvAllocator.GetHeap();
        inputs.viewport = &m_viewport;
        inputs.scissorRect = &m_scissorRect;
        inputs.frameCoordinator = &m_frameCoordinator;
        inputs.frame = frame;

        inputs.lightSystem = &m_lightSystem;
        inputs.frameLight = frame ? &frame->light : nullptr;
        inputs.skybox = &m_skybox;
        inputs.cameraPV = m_cameraState.GetPV();
        inputs.cameraInvPV = m_cameraState.GetInvPV();
        inputs.cameraPos = m_cameraState.GetPos();
        inputs.cameraRight = m_cameraState.GetRight();
        inputs.cameraUp = m_cameraState.GetUp();
        inputs.cameraForward = m_cameraState.GetForward();
        inputs.cameraTanHalfFovY = m_cameraState.GetTanHalfFovY();
        inputs.cameraAspectRatio = m_cameraState.GetAspectRatio();
        inputs.cameraMode = m_cameraState.GetCameraMode();
        inputs.shadowSrv =
            ((m_settings.renderPathMode == RenderPathMode::Raster) &&
             m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled &&
             m_renderTargetPool.GetSWRTShadowTexture().IsValid() &&
             m_swrtExecutor.IsShadowCacheValid())
                ? m_renderTargetPool.GetSWRTShadowSrv()
                : m_lightSystem.GetShadowSrv();
        inputs.spotShadowSrv = m_lightSystem.GetSpotShadowSrv();
        inputs.lightSrvTable = frame ? frame->light.lightSrvTable : GpuDescriptorHandle{};
        inputs.iblSrvTable = m_skybox.GetIblSrvTable();
        inputs.aoSrv = defaultAoSrv;
        inputs.reflectionSrv =
            ((m_settings.renderPathMode == RenderPathMode::Raster) &&
             m_settings.rasterSoftwareRayTracedReflectionEnabled &&
             m_renderTargetPool.GetSWRTReflectionTexture().IsValid() &&
             m_swrtExecutor.IsReflectionCacheValid())
                ? m_renderTargetPool.GetSWRTReflectionSrv()
                : m_nullTextureSrv;
        inputs.lightCbGpu = lightCbGpu;
        inputs.sceneTimeSec = m_sceneTime;

        // SSAO resources
        inputs.depthSrv = m_renderTargetPool.GetDepthSrv();
        inputs.gbufferNormalSrv = m_renderTargetPool.GetGBufferNormal().IsValid()
            ? m_renderTargetPool.GetGBufferNormalSrv()
            : m_nullTextureSrv;
        inputs.depthResource = m_renderTargetPool.GetDepth().IsValid() ? m_renderTargetPool.GetDepth().Get() : nullptr;
        const bool usesSsaoAo = UsesSsaoAmbientOcclusionMode(m_settings.ambientOcclusionMode);
        inputs.ssaoRtv = m_renderTargetPool.GetSSAORtv();
        inputs.ssaoResource =
            (usesSsaoAo && m_renderTargetPool.GetSSAOTexture().IsValid()) ? m_renderTargetPool.GetSSAOTexture().Get() : nullptr;
        inputs.ssaoRawSrv =
            (usesSsaoAo && m_renderTargetPool.GetSSAOTexture().IsValid()) ? m_renderTargetPool.GetSSAOSrv() : m_nullTextureSrv;
        inputs.ssaoBlurRtv = m_renderTargetPool.GetSSAOBlurRtv();
        inputs.ssaoBlurResource =
            (usesSsaoAo && m_renderTargetPool.GetSSAOBlurTexture().IsValid()) ? m_renderTargetPool.GetSSAOBlurTexture().Get() : nullptr;
        switch (m_settings.ambientOcclusionMode) {
        case RendererEnums::AmbientOcclusionMode::MaterialOnly:
            inputs.screenSpaceAoSrv = defaultAoSrv;
            break;
        case RendererEnums::AmbientOcclusionMode::SSAOOnly:
        case RendererEnums::AmbientOcclusionMode::Hybrid:
            inputs.screenSpaceAoSrv =
                m_renderTargetPool.GetSSAOBlurTexture().IsValid() ? m_renderTargetPool.GetSSAOBlurSrv()
                : m_renderTargetPool.GetSSAOTexture().IsValid() ? m_renderTargetPool.GetSSAOSrv()
                : defaultAoSrv;
            break;
        case RendererEnums::AmbientOcclusionMode::SWRTAOOnly:
            inputs.screenSpaceAoSrv =
                m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                    ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                    : defaultAoSrv;
            break;
        default:
            inputs.screenSpaceAoSrv = defaultAoSrv;
            break;
        }

        // Push SSAO CB: reuse PushCameraCB with (cameraPV, cameraInvPV, ssaoParams, screenSize, cameraPos)
        if (frame && usesSsaoAo) {
            const float ssaoExtra0[4] = {
                m_settings.ssaoRadius,
                m_settings.ssaoBias,
                m_settings.ssaoIntensity,
                m_settings.ssaoThickness
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
                static_cast<float>(m_settings.ssaoQuality)
            };
            inputs.ssaoCbGpu = m_frameCoordinator.PushCameraCB(
                *frame, m_cameraState.GetPV(), m_cameraState.GetInvPV(), ssaoExtra0, ssaoExtra1, ssaoExtra2);
        }

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

        if (m_deltaTime > 0.0f) {
            m_sceneTime += m_deltaTime;
        }

        const UINT backIndex = m_device->GetSwapChain()->GetCurrentBackBufferIndex();
        auto* frame = m_frameCoordinator.GetFrameContext(backIndex);
        if (!frame) {
            return;
        }

        CommandList* cmdList = nullptr;
        if (!m_frameCoordinator.BeginFrame(backIndex, cmdList)) {
            return;
        }
        const size_t drawItemCount = m_sceneSubmitter.GetDrawItems().size();
        // Raster shadow rendering records one draw per directional cascade, then the
        // main scene pass records another draw per item. Reserve the spot-shadow pass
        // as well so CSM4 cannot overwrite later per-draw camera constants.
        const size_t cameraCbDrawSlots =
            drawItemCount * (static_cast<size_t>(LightSystem::kDirectionalCascadeCount) + 2u);
        const size_t cameraCbExtraSlots = 8u; // SSAO, sky/fullscreen passes, and small pass variations.
        const size_t cameraCbRequired = cameraCbDrawSlots + cameraCbExtraSlots;
        const UINT cameraCbRequiredClamped = static_cast<UINT>(
            (std::min)(cameraCbRequired, static_cast<size_t>((std::numeric_limits<UINT>::max)())));
        m_frameCoordinator.EnsureCameraBuffers(*frame, cameraCbRequiredClamped);

        EnsureEnvironmentTexturesUploaded(cmdList);

        m_renderGraph.Clear();
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        if (!backBuffer || !m_renderTargetPool.GetDepth().IsValid() || !m_renderTargetPool.GetDepthDsv().ptr) {
            return;
        }

        ExternalRenderGraphResourceDesc sceneColorDesc{};
        sceneColorDesc.resource = backBuffer->Get();
        sceneColorDesc.initialState = D3D12_RESOURCE_STATE_PRESENT;
        sceneColorDesc.finalState = D3D12_RESOURCE_STATE_PRESENT;
        sceneColorDesc.transitionToFinalState = true;
        sceneColorDesc.rtv = m_renderTargetPool.GetBackBufferRtv(backIndex);
        sceneColorDesc.hasRtv = true;
        sceneColorDesc.clearColorOnFirstUse = true;
        sceneColorDesc.clearColor[0] = 0.2f;
        sceneColorDesc.clearColor[1] = 0.2f;
        sceneColorDesc.clearColor[2] = 0.2f;
        sceneColorDesc.clearColor[3] = 1.0f;
        m_renderGraph.ImportExternalResource("SceneColor", sceneColorDesc);

        ExternalRenderGraphResourceDesc sceneDepthDesc{};
        sceneDepthDesc.resource = m_renderTargetPool.GetDepth().Get();
        sceneDepthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        sceneDepthDesc.finalState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        sceneDepthDesc.transitionToFinalState = true;
        sceneDepthDesc.dsv = m_renderTargetPool.GetDepthDsv();
        sceneDepthDesc.hasDsv = true;
        sceneDepthDesc.clearDepthOnFirstUse = true;
        sceneDepthDesc.clearDepth = 1.0f;
        sceneDepthDesc.clearStencil = 0;
        m_renderGraph.ImportExternalResource("SceneDepth", sceneDepthDesc);

        // Import GBuffer resources so LightingRenderNode can bind them as MRTs.
        const UINT gbufferW = static_cast<UINT>(m_viewport.Width);
        const UINT gbufferH = static_cast<UINT>(m_viewport.Height);
        m_renderTargetPool.EnsureGBuffer(*m_device, gbufferW, gbufferH);
        if (m_renderTargetPool.GetGBufferAlbedo().IsValid()) {
            const auto importGBuffer = [&](const char* name, Resource& res, CpuDescriptorHandle rtv) {
                ExternalRenderGraphResourceDesc d{};
                d.resource = res.Get();
                d.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                d.finalState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                d.transitionToFinalState = true;
                d.rtv = rtv;
                d.hasRtv = true;
                d.clearColorOnFirstUse = true;
                // clearColor defaults to 0,0,0,0
                m_renderGraph.ImportExternalResource(name, d);
            };
            importGBuffer("GBufferAlbedo",   m_renderTargetPool.GetGBufferAlbedo(),   m_renderTargetPool.GetGBufferAlbedoRtv());
            importGBuffer("GBufferNormal",   m_renderTargetPool.GetGBufferNormal(),   m_renderTargetPool.GetGBufferNormalRtv());
            importGBuffer("GBufferMaterial", m_renderTargetPool.GetGBufferMaterial(), m_renderTargetPool.GetGBufferMaterialRtv());
            importGBuffer("GBufferEmissive", m_renderTargetPool.GetGBufferEmissive(), m_renderTargetPool.GetGBufferEmissiveRtv());
        }

        auto drawItems = [this, cmdList, frame](bool drawTransparent) {
            // Bind screen-space AO texture at slot 8 (t9 in shader) once per batch
            // Use blur output if available, fall back to raw SSAO, then null
            GpuDescriptorHandle ssaoSrv = m_nullTextureSrv;
            switch (m_settings.ambientOcclusionMode) {
            case RendererEnums::AmbientOcclusionMode::MaterialOnly:
                ssaoSrv = m_nullTextureSrv;
                break;
            case RendererEnums::AmbientOcclusionMode::SSAOOnly:
            case RendererEnums::AmbientOcclusionMode::Hybrid:
            {
                // Fallback to white (AO=1) rather than null (AO=0) when SSAO textures aren't ready.
                // Using a black null texture here would zero all ambient even before SSAO runs.
                const GpuDescriptorHandle ssaoFallback = m_defaultOcclusionTexture
                    ? m_defaultOcclusionTexture->srv : m_nullTextureSrv;
                if (m_settings.gBufferDebugView == RendererEnums::GBufferDebugView::ScreenSpaceAmbientOcclusionRaw) {
                    ssaoSrv = m_renderTargetPool.GetSSAOTexture().IsValid()
                        ? m_renderTargetPool.GetSSAOSrv()
                        : ssaoFallback;
                } else {
                    ssaoSrv =
                        m_renderTargetPool.GetSSAOBlurTexture().IsValid() ? m_renderTargetPool.GetSSAOBlurSrv()
                        : m_renderTargetPool.GetSSAOTexture().IsValid() ? m_renderTargetPool.GetSSAOSrv()
                        : ssaoFallback;
                }
                break;
            }
            case RendererEnums::AmbientOcclusionMode::SWRTAOOnly:
                ssaoSrv =
                    m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                        ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                        : m_nullTextureSrv;
                break;
            default:
                break;
            }
            cmdList->SetGraphicsRootDescriptorTable(8, ssaoSrv);

            // Bind GI probe grid CB (b2) and probe SH data (t10) as inline root descriptors.
            // These are always bound so PBR_PS can read g_giEnabled to decide whether to use probes.
            if (m_probeGrid.IsInitialized()) {
                const D3D12_GPU_VIRTUAL_ADDRESS probeCbGpu = m_probeGrid.GetProbeGridCbGpuAddress();
                const D3D12_GPU_VIRTUAL_ADDRESS probeVA    = m_probeGrid.GetProbeDataGpuVA();
                if (probeCbGpu != 0) cmdList->SetGraphicsRootConstantBufferView(9, probeCbGpu);
                if (probeVA    != 0) cmdList->SetGraphicsRootShaderResourceView(10, probeVA);
            }

            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.transparent != drawTransparent) {
                    continue;
                }

                const float extra0[4] = {
                    item.material.baseColor[0],
                    item.material.baseColor[1],
                    item.material.baseColor[2],
                    item.material.baseColor[3],
                };
                const float extra1[4] = {
                    item.material.emissive[0],
                    item.material.emissive[1],
                    item.material.emissive[2],
                    item.material.roughness,
                };
                const float extra2[4] = {
                    item.material.metallic,
                    item.material.occlusionStrength,
                    static_cast<float>(m_settings.ambientOcclusionMode),
                    item.usesMetallicRoughnessTexture ? 1.0f : 0.0f,
                };
                float objMVP[16];
                Mul4x4(item.model, m_cameraState.GetPV(), objMVP);
                const D3D12_GPU_VIRTUAL_ADDRESS cameraCbGpu =
                    m_frameCoordinator.PushCameraCB(*frame, objMVP, item.model, extra0, extra1, extra2);
                if (cameraCbGpu != 0) {
                    cmdList->SetGraphicsRootConstantBufferView(2, cameraCbGpu);
                }

                if (item.texture) {
                    cmdList->SetGraphicsRootDescriptorTable(0, item.texture->srv);
                } else if (m_defaultAlbedoTexture) {
                    cmdList->SetGraphicsRootDescriptorTable(0, m_defaultAlbedoTexture->srv);
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
        const SWRTExecutor::PartialBehavior partialBehavior =
            m_swrtExecutor.ResolveBehavior(m_settings.rayTracingPerformancePreset);
        const bool useSoftwareRayTracedDirectionalShadow =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled;
        const bool useSoftwareRayTracedReflections =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled;
        const bool useSoftwareRayTracedAmbientOcclusion =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            UsesSwrtAmbientOcclusionMode(m_settings.ambientOcclusionMode);
        if (useSoftwareRayTracedDirectionalShadow) {
            bool cacheInvalidated = false;
            if (!m_renderTargetPool.EnsureSWRTShadow(*m_device, partialBehavior.shadowMapSize, cacheInvalidated)) {
                DebugLog("Renderer::Render: failed to prepare software directional shadow resources. Falling back to raster shadow map.\n");
            }
            if (cacheInvalidated) {
                m_swrtExecutor.OnShadowResourcesReallocated();
            }
        }
        if (useSoftwareRayTracedReflections) {
            const uint32_t reflectionWidth  = ComputeScaledDimension(
                static_cast<uint32_t>(m_viewport.Width),  partialBehavior.reflectionResolutionScale);
            const uint32_t reflectionHeight = ComputeScaledDimension(
                static_cast<uint32_t>(m_viewport.Height), partialBehavior.reflectionResolutionScale);
            bool reflCacheInvalidated = false;
            if (!m_renderTargetPool.EnsureSWRTReflection(*m_device, reflectionWidth, reflectionHeight, reflCacheInvalidated)) {
                DebugLog("Renderer::Render: failed to prepare software reflection resources. Disabling SWRT reflections for this frame.\n");
            }
            if (reflCacheInvalidated) {
                m_swrtExecutor.OnReflectionResourcesReallocated();
            }
        }
        if (useSoftwareRayTracedAmbientOcclusion) {
            bool aoCacheInvalidated = false;
            if (!m_renderTargetPool.EnsureSWRTAmbientOcclusion(*m_device,
                                                               static_cast<uint32_t>(m_viewport.Width),
                                                               static_cast<uint32_t>(m_viewport.Height),
                                                               aoCacheInvalidated)) {
                DebugLog("Renderer::Render: failed to prepare software ambient occlusion resources. Falling back to SSAO.\n");
            }
        }
        if (useSoftwareRayTracedDirectionalShadow || useSoftwareRayTracedReflections || useSoftwareRayTracedAmbientOcclusion) {
            m_rayTracingStats = {};
            m_rayTracingStats.hardwareSupported     = IsHardwareRayTracingSupported();
            m_rayTracingStats.instanceCount         = static_cast<uint32_t>(m_rayTracingScene.instances.size());
            m_rayTracingStats.triangleCount         = m_rayTracingScene.TriangleCount();
            m_rayTracingStats.shadowMapSize         = partialBehavior.shadowMapSize;
            m_rayTracingStats.reflectionWidth       = m_renderTargetPool.GetSWRTReflectionWidth();
            m_rayTracingStats.reflectionHeight      = m_renderTargetPool.GetSWRTReflectionHeight();
            m_rayTracingStats.shadowUpdateInterval  = partialBehavior.shadowUpdateInterval;
            m_rayTracingStats.reflectionUpdateInterval = partialBehavior.reflectionUpdateInterval;
            m_rayTracingStats.reflectionPhaseCount  = partialBehavior.reflectionPhaseCount;
            m_rayTracingStats.reflectionPhaseIndex  = m_swrtExecutor.GetReflectionPhaseIndex();
            m_rayTracingStats.reflectionResolutionScale = partialBehavior.reflectionResolutionScale;
            m_rayTracingStats.reflectionMaxRoughness = partialBehavior.reflectionMaxRoughness;
            m_rayTracingStats.reflectionMinEnergy   = partialBehavior.reflectionMinEnergy;
            m_rayTracingStats.reflectionMaxDistance = partialBehavior.reflectionMaxTraceDistance;
        }

        // SdfFluid mode bypasses all rasterized scene passes
        const bool isSdfFluidMode = (m_settings.renderPathMode == RenderPathMode::SdfFluid);
        const bool executeOpaqueFamilyPasses = !isSdfFluidMode && (m_settings.rasterShaderMode == RasterShaderMode::Opaque);
        const bool executeLightingFamilyPasses = !isSdfFluidMode && !executeOpaqueFamilyPasses;
        const bool hasLightingPass =
            executeLightingFamilyPasses &&
            std::any_of(m_passRegistry.GetPasses().begin(),
                        m_passRegistry.GetPasses().end(),
                        [](const std::shared_ptr<IRenderNode>& runtimeNode) {
                            if (!runtimeNode) {
                                return false;
                            }
                            const std::string_view tag = runtimeNode->Tag();
                            return tag == "Lighting" || tag == "TransparentLighting";
                        });
        const bool useShadowTessPath = hasLightingPass && m_settings.useTessellation;

        auto drawShadowItems = [this, cmdList, frame](const LightSystem::ShadowPassContext& context) {
            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
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
        // --- Build frame inputs for graphics and (optionally) compute queues ---
        RenderNodeFrameInputs frameInputs = BuildRenderNodeFrameInputs(
            cmdList,
            frame,
            lightCbGpu,
            defaultAoSrv);

        // Prepare compute command list if async compute is available.
        CommandList* computeCmdList = nullptr;
        RenderNodeFrameInputs computeFrameInputs{};
        if (m_computeCmdListReady && !m_computeAllocators.empty()) {
            CommandAllocator& computeAlloc = m_computeAllocators[backIndex % m_computeAllocators.size()];
            computeAlloc.Reset();
            if (SUCCEEDED(m_computeCmdList.Reset(computeAlloc, nullptr))) {
                computeCmdList = &m_computeCmdList;
                computeFrameInputs = frameInputs;
                computeFrameInputs.cmdList = computeCmdList;
                frameInputs.computeCmdList = computeCmdList;
            }
        }

        RenderNodeExecutionServices executionServices = BuildRenderNodeExecutionServices(
            drawItems,
            drawShadowItems);
        executionServices.executeSoftwareDirectionalShadow = [this, cmdList, partialBehavior](const LightSystem::ShadowPassContext& shadowContext) {
            const auto ctx = BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteDirectionalShadow(cmdList, shadowContext, ctx, partialBehavior, m_settings, m_rayTracingStats);
        };
        executionServices.executeSoftwareReflections = [this, cmdList, partialBehavior]() {
            const auto ctx = BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteReflections(cmdList, ctx, partialBehavior, m_settings, m_rayTracingStats);
        };
        executionServices.executeRayTracing = [this, cmdList, backIndex]() {
            const auto ctx = BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteHardware(cmdList, backIndex, ctx, m_settings, m_rayTracingStats);
        };

        m_passRegistry.ClearPhaseCompletionNodes();
        if (useSoftwareRayTracedAmbientOcclusion) {
            m_passRegistry.AddPhaseCompletionNode(
                "Scene",
                "SwrtAmbientOcclusion",
                [this, cmdList](const RenderNodeContextView&) -> bool {
                    const auto ctx = BuildSwrtFrameContext();
                    return m_swrtExecutor.ExecuteAmbientOcclusion(cmdList, ctx, m_settings, m_rayTracingStats);
                },
                PhaseCompletionMode::Deterministic,
                {});
        }
        if (useSoftwareRayTracedReflections) {
            m_passRegistry.AddPhaseCompletionNode(
                "Scene",
                "SwrtReflections",
                [this, cmdList, partialBehavior](const RenderNodeContextView&) -> bool {
                    const auto ctx = BuildSwrtFrameContext();
                    return m_swrtExecutor.ExecuteReflections(cmdList, ctx, partialBehavior, m_settings, m_rayTracingStats);
                },
                PhaseCompletionMode::Deterministic,
                {});
        }

        RenderGraphExecuteContext executeContext{};
        executeContext.executionPolicy    = &executionPolicy;
        executeContext.frameInputs        = &frameInputs;
        executeContext.computeFrameInputs = computeCmdList ? &computeFrameInputs : nullptr;
        executeContext.executionServices  = &executionServices;
        executeContext.resources          = &m_renderGraph.GetResourceRegistry();
        if (computeCmdList && m_crossQueueFence) {
            executeContext.graphicsQueueRaw   = m_device->GetCommandQueue().Get();
            executeContext.computeQueueRaw    = m_device->GetComputeQueue().Get();
            executeContext.crossQueueFence    = m_crossQueueFence.Get();
            executeContext.crossQueueFenceVal = &m_crossQueueFenceVal;
        }

        if (!m_passRegistry.RegisterPassesToRenderGraph(m_renderGraph, executeContext, m_settings.renderPathMode, m_rayTracingRenderNode)) {
            m_renderGraph.Clear();
            if (overlay) {
                TransitionBackBufferToRenderTarget(cmdList, backIndex);
                ClearAndBindMainTargets(cmdList, backIndex);
                overlay(*cmdList, m_renderTargetPool.GetBackBufferRtv(backIndex));
                TransitionBackBufferToPresent(cmdList, backIndex);
            }
            SubmitAndPresent(cmdList, backIndex);
            return;
        }

        m_passRegistry.RegisterPhaseCompletionNodesToRenderGraph(m_renderGraph, executeContext);

        const bool graphExecuted = m_renderGraph.Execute();
        m_renderGraph.Clear();

        // Submit compute CL (SWRT work) on the compute queue.
        // The cross-queue fence sync is set up by RenderGraph::Execute() — this submission
        // happens after the Signal/Wait calls that ensure GPU ordering.
        if (computeCmdList) {
            if (SUCCEEDED(computeCmdList->Close())) {
                ID3D12CommandList* cLists[] = { computeCmdList->Get() };
                m_device->GetComputeQueue().ExecuteCommandLists(1, cLists);
            }
        }

        if (!graphExecuted) {
            if (overlay) {
                TransitionBackBufferToRenderTarget(cmdList, backIndex);
                BindMainTargets(cmdList, backIndex);
                overlay(*cmdList, m_renderTargetPool.GetBackBufferRtv(backIndex));
                TransitionBackBufferToPresent(cmdList, backIndex);
            }
            SubmitAndPresent(cmdList, backIndex);
            return;
        }

        if (overlay) {
            TransitionBackBufferToRenderTarget(cmdList, backIndex);
            BindMainTargets(cmdList, backIndex);
            overlay(*cmdList, m_renderTargetPool.GetBackBufferRtv(backIndex));
            TransitionBackBufferToPresent(cmdList, backIndex);
        }
        SubmitAndPresent(cmdList, backIndex);
    }
    Renderer::DirectionalLightSettings Renderer::GetDirectionalLightSettings() const
    {
        return m_lightSystem.GetDirectionalLightSettings();
    }

    bool Renderer::IsHardwareRayTracingSupported() const
    {
        return m_device ? m_device->SupportsHardwareRayTracing() : false;
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

        m_renderTargetPool.ReleaseBackBuffers();

        HRESULT hr = m_device->GetSwapChain()->ResizeBuffers(GetBackBufferCount(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(hr)) {
            DebugLog("ResizeBuffers failed\n");
            return;
        }

        if (!m_renderTargetPool.InitializeBackBuffers(*m_device, m_device->GetSwapChain(), GetBackBufferCount())) {
            DebugLog("Back buffer target rebuild failed\n");
            return;
        }

        m_viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

        m_renderTargetPool.OnResize(*m_device, width, height);

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
        m_skybox.RefreshEnvironmentAssets();
        m_swrtExecutor.InvalidateCache();
    }

    void Renderer::SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height)
    {
        m_skybox.SetHdrEquirectData(std::move(pixels), width, height);
        m_swrtExecutor.OnReflectionResourcesReallocated();
    }

    void Renderer::SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height)
    {
        m_skybox.SetLdrEquirectData(std::move(pixels), width, height);
        m_swrtExecutor.OnReflectionResourcesReallocated();
    }

    void Renderer::SetSkyboxLoadFormat(SkyboxLoadFormat format)
    {
        m_skybox.SetLoadFormat(format);
        m_swrtExecutor.OnReflectionResourcesReallocated();
    }

    Renderer::PassHandle Renderer::AddPass(const std::shared_ptr<IRenderNode>& renderPass)
    {
        return m_passRegistry.AddPass(renderPass);
    }

    Renderer::PassHandle Renderer::AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        return m_passRegistry.AddPassBefore(targetTag, renderPass);
    }

    Renderer::PassHandle Renderer::AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        return m_passRegistry.AddPassAfter(targetTag, renderPass);
    }

    bool Renderer::ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        return m_passRegistry.ReplacePass(targetTag, renderPass);
    }

    bool Renderer::AddPhaseCompletionNode(std::string_view phaseTag,
                                          std::string_view nodeName,
                                          const PhaseCompletionCallback& execute,
                                          PhaseCompletionMode mode,
                                          const RenderNodeRequirements& requirements)
    {
        return m_passRegistry.AddPhaseCompletionNode(phaseTag, nodeName, execute, mode, requirements);
    }

    void Renderer::ClearPhaseCompletionNodes()
    {
        m_passRegistry.ClearPhaseCompletionNodes();
    }

    void Renderer::ClearPasses()
    {
        m_passRegistry.ClearPasses();
    }

    void Renderer::FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                        float bMaxX, float bMaxY, float bMaxZ,
                                        float margin)
    {
        m_probeGrid.FitToSceneBounds(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);
        if (m_probeGrid.IsInitialized() && m_device) {
            m_probeGrid.ReallocProbeBuffer(*m_device);
        }
    }

    void Renderer::SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence)
    {
        m_passRegistry.SetRenderNodeSequence(sequence);

        // Re-insert non-builtin nodes that were previously added via AddPassBefore/AddPassAfter.
        // SetRenderNodeSequence calls ClearPasses() internally, which removes them.
        // Keep the probe grid after Lighting/Skybox so deferred PBR has already produced SceneColor.
        if (m_debugProbeGridRenderNode && m_debugProbeGridRenderNode->IsInitialized()) {
            if (!AddPassAfter("Skybox", m_debugProbeGridRenderNode).IsValid()) {
                if (!AddPassAfter("Lighting", m_debugProbeGridRenderNode).IsValid()) {
                    AddPass(m_debugProbeGridRenderNode);
                }
            }
        }
    }

    void Renderer::ReinsertDebugProbeGrid()
    {
        if (!m_debugProbeGridRenderNode || !m_debugProbeGridRenderNode->IsInitialized()) return;
        if (AddPassAfter("Skybox", m_debugProbeGridRenderNode).IsValid()) return;
        if (AddPassAfter("Lighting", m_debugProbeGridRenderNode).IsValid()) return;
        AddPass(m_debugProbeGridRenderNode);
    }

    void Renderer::TransitionBackBufferToRenderTarget(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        barrier.Transition.pResource = backBuffer ? backBuffer->Get() : nullptr;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void Renderer::BindMainTargets(CommandList* cmdList, UINT backIndex)
    {
        auto rtvHandle = m_renderTargetPool.GetBackBufferRtv(backIndex);
        auto dsvMain = m_renderTargetPool.GetDepthDsv();
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvMain);
    }

    void Renderer::ClearAndBindMainTargets(CommandList* cmdList, UINT backIndex)
    {
        BindMainTargets(cmdList, backIndex);
        auto rtvHandle = m_renderTargetPool.GetBackBufferRtv(backIndex);
        auto dsvMain = m_renderTargetPool.GetDepthDsv();
        const FLOAT clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(dsvMain, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    // SWRT execution methods moved to SWRTExecutor.cpp

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
        if (!m_srvAllocator.Allocate(1, cpu, gpu)) {
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
        m_defaultTextures.push_back(std::move(texObj));
        return m_defaultTextures.back().get();
    }

    void Renderer::TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
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
        m_cameraState.Update(camera);
    }

    void Renderer::SubmitRenderProxies(std::vector<RenderProxy>&& proxies)
    {
        m_sceneSubmitter.SubmitRenderProxies(std::move(proxies));
    }

    void Renderer::ClearSubmittedRenderProxies()
    {
        m_sceneSubmitter.ClearSubmittedRenderProxies();
        m_swrtExecutor.InvalidateCache();
    }

    void Renderer::ClearRenderObjects()
    {
        ClearSubmittedRenderProxies();
    }
}
