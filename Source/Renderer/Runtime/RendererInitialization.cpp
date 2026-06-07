// RendererInitialization.cpp
// Renderer::Initialize  Edevice creation, pipeline setup, resource allocation.
// Extracted from Renderer.cpp to keep the main file focused on the render loop.
#define NOMINMAX
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/SceneSynchronizer.h"
#include "Renderer/Scene/EnvironmentManager.h"

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
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "d3dx12.h"

using namespace std;

namespace SasamiRenderer
{
    namespace
    {
        bool UsesNativeBackendFrame(const IRHIDevice& device)
        {
            return !device.GetCapabilities().supportsFeatureRenderPasses &&
                   device.GetCapabilities().supportsNativeFrame;
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
            if (UsesNativeBackendFrame(*m_device)) {
                m_viewport = { 0.0f, 0.0f, static_cast<float>(clientW), static_cast<float>(clientH), 0.0f, 1.0f };
                m_scissorRect = { 0, 0, static_cast<LONG>(clientW), static_cast<LONG>(clientH) };
                SceneSubmitter::InitParams submitterParams{};
                submitterParams.device             = m_device.get();
                submitterParams.meshBuffer         = &m_meshBuffer;
                submitterParams.skinnedMeshBuffer  = &m_skinnedMeshBuffer;
                submitterParams.rayTracingScene    = &m_rayTracingScene;
                submitterParams.dxrRayTracer       = &m_dxrRayTracer;
                submitterParams.srvAllocFn         = [](UINT, CpuDescriptorHandle&, GpuDescriptorHandle&) {
                    return false;
                };
                submitterParams.srvIndexFn         = [](GpuDescriptorHandle) {
                    return 0u;
                };
                m_sceneSubmitter.Initialize(submitterParams);
                DebugLog("Renderer::Initialize: native backend initialized. RayMarch and static mesh samples use backend native paths; D3D12 feature render passes are not used on this backend yet.\n");
                return true;
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
            ApplySwrtModeSetting();
            if (!m_probeGrid.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: IrradianceProbeGrid initialization failed.\n");
            }
            if (m_debugProbeGridRenderPass) {
                if (!m_debugProbeGridRenderPass->Initialize(*m_device)) {
                    DebugLog("Renderer::Initialize: DebugProbeGridRenderPass initialization failed (non-fatal).\n");
                } else {
                    m_debugProbeGridRenderPass->SetProbeGrid(&m_probeGrid);
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
            submitterParams.device             = m_device.get();
            submitterParams.meshBuffer         = &m_meshBuffer;
            submitterParams.skinnedMeshBuffer  = &m_skinnedMeshBuffer;
            submitterParams.rayTracingScene    = &m_rayTracingScene;
            submitterParams.dxrRayTracer       = &m_dxrRayTracer;
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

        m_passRegistry.SetBuiltinPasses({
            m_shadowRenderPass,
            m_opaqueRenderPass,
            m_lightingRenderPass,
            m_transparentRenderPass,
            m_transparentLightingRenderPass,
            m_skyboxRenderPass,
            m_postProcessRenderPass,
            m_ssaoRenderPass,
            m_proceduralSkyRenderPass,
            m_transparentBackfaceDistanceRenderPass,
            m_transparentCompositeRenderPass,
            m_ssaoBlurRenderPass,
            m_transparentSceneColorCopyRenderPass,
            m_softwareReflectionRenderPass,
            m_softwareReflectionCompositeRenderPass
        });

        EnsureVolumetricCloudPassInserted();

        // Insert the debug probe grid after sky composition so deferred lighting has already
        // produced SceneColor. Transparent passes still come later and can draw over it.
        if (m_debugProbeGridRenderPass && m_debugProbeGridRenderPass->IsInitialized()) {
            if (!AddPassAfter("Skybox", m_debugProbeGridRenderPass).IsValid()) {
                if (!AddPassAfter("Lighting", m_debugProbeGridRenderPass).IsValid()) {
                    AddPass(m_debugProbeGridRenderPass);
                }
            }
        }

        return true;
    }


} // namespace SasamiRenderer
