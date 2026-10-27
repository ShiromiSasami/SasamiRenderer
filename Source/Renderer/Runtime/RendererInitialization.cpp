// RendererInitialization.cpp
// Renderer initialization: device creation, pipeline setup, resource allocation.
// Extracted from Renderer.cpp to keep the main file focused on the render loop.
//
// Split into InitializeCore / InitializeFrameInfrastructure / FinalizeCoreInitialization /
// RegisterDeferredInitTasks for progressive boot (see Renderer.h). Initialize() below
// remains the synchronous composite of all four for callers that don't need progressive
// boot (runtime backend switching, tools).
#define NOMINMAX
#include "Renderer/Runtime/NativeBackendFrame.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/SceneSynchronizer.h"
#include "Renderer/Scene/EnvironmentManager.h"
#include "Foundation/Boot/InitTaskScheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include "Renderer/Passes/Geometry/ToonOutlineRenderPass.h"
#include "d3dx12.h"

#include <cstdio>
#include <cstdlib>

using namespace std;

namespace SasamiEngine::Render
{
    namespace
    {
        // GetEnvironmentVariableA rather than std::getenv, matching SASAMI_GPU_VALIDATION.
        uint64_t ClusterPagePoolBytes()
        {
            constexpr uint64_t kDefaultMegabytes = 96ull;

            char megabytesEnv[16] = {};
            GetEnvironmentVariableA("SASAMI_PAGE_POOL_MB", megabytesEnv, sizeof(megabytesEnv));

            uint64_t megabytes = kDefaultMegabytes;
            if (megabytesEnv[0] != '\0') {
                const uint64_t parsed = std::strtoull(megabytesEnv, nullptr, 10);
                if (parsed > 0ull) {
                    megabytes = parsed;
                    char message[128];
                    std::snprintf(message, sizeof(message),
                                  "Renderer::Initialize: ClusterPagePool size overridden to %llu MB by SASAMI_PAGE_POOL_MB.\n",
                                  static_cast<unsigned long long>(megabytes));
                    DebugLog(message);
                }
            }
            return megabytes << 20;
        }
    }

    bool Renderer::InitializeCore(HWND hWnd, UINT width, UINT height)
    {
        ScopedPerfTimer perfTimer("Renderer::InitializeCore");
        auto failInit = [](const char* message) -> bool {
            DebugLogDialog(message, L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
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

            // Resolve once here: the device exists now, so its reported capabilities are real.
            m_featureSupport.Resolve(m_device->GetCapabilities());

            // Same reason the constructor could not: HardwareRayTracingPassBuilder gates on
            // supportsHardwareRayTracing, and only the device knows it. Build returns null when the
            // builder reports unavailable, which leaves m_rayTracingRenderPass null on a device
            // without DXR -- the honest answer, where the constructor's placeholder would have
            // produced a pass that cannot run.
            RenderPassBuildContext rayTracingContext{};
            rayTracingContext.capabilities = m_device->GetCapabilities();
            m_rayTracingRenderPass = std::dynamic_pointer_cast<RayTracingRenderPass>(
                m_renderPassBuilderCatalog.Build(RenderPassBuilderId::RayTracing, rayTracingContext));
            if (!m_rayTracingRenderPass) {
                DebugLog("Renderer::InitializeCore: hardware ray tracing unsupported; the ray tracing pass was not built.\n");
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
                // The full Skybox::Initialize below is never reached on this path, but the native
                // sky still needs the device to build its own environment cubemap from the HDR
                // source (Skybox::EnsureNativeEnvironmentCubemap).
                m_skybox.InitializeNativeOnly(*m_device);
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
            // SrvDescriptorAllocator is a bump allocator with no free list, so this is a
            // hard ceiling on every SRV/UAV the renderer will ever hand out: scene material
            // textures plus one-off consumers such as SkyCubemapGenerator's mip chain.
            // 512 was not enough for Bistro -- the scene alone consumed ~490, and the sky
            // cubemap generator's 24 descriptors (1 equirect SRV + 12 mip UAVs + 11 mip
            // SRVs at a 2048 face) then pushed it past the end. That failure is silent from
            // the user's side: Skybox falls back to the CPU cubemap path and the load takes
            // ~14.6s instead of the GPU path's dispatches. A shader-visible CBV/SRV/UAV heap
            // costs 32 bytes per descriptor, so 4096 slots is 128KB -- cheap insurance
            // against a scene one texture larger silently costing 14 seconds again.
            // Raised to 16384: bindless mesh vertex-buffer SRVs for the cluster path need one
            // per mesh (Bistro ~1700), on top of the existing scene/skybox allocations.
            if (!m_srvAllocator.Initialize(*m_device, 16384)) {
                return failInit("Renderer::Initialize: SRV descriptor heap creation failed.\n");
            }
        }

        {
            ScopedPerfTimer stepTimer("Renderer::Initialize.ClusterGpuCulling");
            // Non-fatal: the GPU-driven cluster path falls back to the CPU-driven one
            // (ClusterDrawSubmitter::DrawShadow) when either subsystem fails to initialize.
            if (!m_clusterGpuCulling.Initialize(*m_device, GetBackBufferCount())) {
                DebugLog("Renderer::Initialize: ClusterGpuCulling initialization failed; falling back to the CPU-driven cluster shadow path.\n");
            }
            // 8192 is the initial per-frame-slot capacity, not a ceiling: ClusterInstanceBuffer
            // reallocates the slot it is about to write when the draw list outgrows it. Kept at the
            // value the fixed-size buffer used so the shipped scenes allocate exactly what they did
            // before (Bistro draws ~1697 instances, so 768 KB x 3 slots is never reallocated), while
            // a 100k-instance procedural scene grows to 131072 (12 MB per slot) on its first frame
            // instead of silently dropping 11 of every 12 instances.
            if (!m_clusterInstanceBuffer.Initialize(*m_device, GetBackBufferCount(), 8192)) {
                DebugLog("Renderer::Initialize: ClusterInstanceBuffer initialization failed; falling back to the CPU-driven cluster shadow path.\n");
            }
            // Same initial capacity as the instance list it parallels (8192 x 128 B = 1 MB per slot).
            if (!m_clusterMaterialTable.Initialize(*m_device, GetBackBufferCount(), 8192)) {
                DebugLog("Renderer::Initialize: ClusterMaterialTable initialization failed; the GPU-driven main view will have no material table.\n");
            }
            // 96 MB by default: Bistro's full page set is 52 MB, so nothing is ever deferred there.
            // SASAMI_PAGE_POOL_MB shrinks it so the exhaustion and fragmentation paths can actually be
            // exercised -- render.poolstats splits deferrals by cause, and a counter that never fires
            // is a counter nobody has tested.
            if (!m_clusterPagePool.Initialize(*m_device, GetBackBufferCount(), ClusterPagePoolBytes(), 16ull << 20 /* 16 MB ring per frame */)) {
                DebugLog("Renderer::Initialize: ClusterPagePool initialization failed; falling back to the CPU-driven cluster shadow path.\n");
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
            if (!m_skybox.Initialize(*m_device, allocateSrv, m_srvAllocator.GetHeap())) {
                return failInit("Renderer::Initialize: Skybox initialization failed.\n");
            }
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
            // NOTE: this sink calls m_frameCoordinator.SignalQueueFence(). It is only
            // invoked lazily (on later uploads), by which point InitializeFrameInfrastructure
            // has always run -- see Initialize()'s composite ordering.
            submitterParams.deferredUploadSink = [this](CommandAllocator&& allocator, CommandList&& commandList,
                                                        std::vector<Resource>&& uploadResources) {
                DeferredUploadBatch batch;
                batch.allocator       = std::move(allocator);
                batch.commandList     = std::move(commandList);
                batch.uploadResources = std::move(uploadResources);
                batch.retireFenceValue = m_frameCoordinator.SignalQueueFence();
                if (batch.retireFenceValue == 0) {
                    m_device->WaitForGPU();
                    return;
                }
                m_deferredUploadBatches.push_back(std::move(batch));
            };
            m_sceneSubmitter.Initialize(submitterParams);
        }

        // Positional: RenderPassRegistry indexes this array with the RenderPassType value
        // itself (BuildPassesFromSequence), so every slot must line up with its enumerator.
        // Values 1 (Opaque) and 3 (Transparent) are retired unlit passes -- hold their
        // slots with nullptr instead of closing the gaps, or every later pass shifts.
        m_passRegistry.SetBuiltinPasses({
            m_shadowRenderPass,          // 0  Shadow
            nullptr,                     // 1  (retired: unlit Opaque)
            m_lightingRenderPass,        // 2  Lighting
            nullptr,                     // 3  (retired: unlit Transparent)
            m_transparentLightingRenderPass,
            m_skyboxRenderPass,
            m_postProcessRenderPass,
            m_ssaoRenderPass,
            m_proceduralSkyRenderPass,
            m_transparentBackfaceDistanceRenderPass,
            m_transparentCompositeRenderPass,
            m_ssaoBlurRenderPass,
            m_transparentSceneColorCopyRenderPass,
            m_screenSpaceReflectionRenderPass,
            m_softwareReflectionRenderPass,
            m_softwareReflectionCompositeRenderPass,
            m_opaqueGBufferRenderPass,
            m_screenSpaceReflectionCompositeRenderPass,
            // 18 ToonOutline. This list must cover every slot up to kBuiltinPassCount: it
            // overwrites the catalog-built array from the Renderer constructor, so a short list
            // silently leaves the tail nullptr. It used to stop at 17, which made
            // UseToonRenderNodePreset() drop the outline pass with only a log line -- ToonApp
            // rendered without outlines. The pass is stateless (see ToonOutlineRenderPass), so
            // constructing it here matches what StatelessRenderPassBuilder would have produced.
            std::make_shared<ToonOutlineRenderPass>()
        });

        // VolumetricCloud's pipelines are built by RenderPipelineStateCache::Initialize
        // above, so the pass can be inserted here without waiting for any deferred task.
        EnsureVolumetricCloudPassInserted();
        // Same reasoning: the mesh-shader pipeline (if the hardware supports it) is also
        // built by RenderPipelineStateCache::Initialize above.
        EnsureMeshShaderPassInserted();

        return true;
    }

    bool Renderer::InitializeFrameInfrastructure()
    {
        ScopedPerfTimer perfTimer("Renderer::InitializeFrameInfrastructure");
        ScopedPerfTimer stepTimer("Renderer::Initialize.FrameCoordinator");
        // Both things set up below are D3D12-only: the frame coordinator allocates D3D12 command
        // allocators and lists, and the timestamp profiler needs an ID3D12QueryHeap. A native
        // backend frame never touches either -- it goes through ExecuteBackendFrame -- so on those
        // backends this whole step is skipped rather than failing boot on E_NOTIMPL.
        if (UsesNativeBackendFrame(*m_device)) {
            DebugLog("Renderer::InitializeFrameInfrastructure: skipped, backend uses its own frame path.\n");
            return true;
        }

        auto allocateSrv = [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) -> bool {
            return m_srvAllocator.Allocate(count, outCpu, outGpu);
        };
        if (!m_frameCoordinator.Initialize(*m_device,
                                           m_pipelineStateCache,
                                           m_lightSystem,
                                           GetBackBufferCount(),
                                           allocateSrv)) {
            // Runs on a JobSystem worker when supportsThreadedResourceCreation -- no modal
            // dialog here; the coordinator (main thread) shows failInit on false return.
            DebugLog("Renderer::InitializeFrameInfrastructure: Frame context initialization failed.\n");
            return false;
        }

        // GPU timing is a diagnostic: a failure here must not fail renderer init. The
        // profiler reports IsReady() == false and every Begin/End/Update call no-ops.
        if (!m_gpuTimestampProfiler.Initialize(m_device->GetDevice(), m_device->GetCommandQueue().Get())) {
            DebugLog("Renderer::InitializeFrameInfrastructure: GPU timestamp profiler unavailable (timing disabled).\n");
        }
        return true;
    }

    // Publishes the "this subsystem finished initialising" flags that gate boot progress.
    void Renderer::MarkRasterSubsystemsReady()
    {
        m_readyState.coreReady.store(true, std::memory_order_release);
        m_readyState.rasterReady.store(true, std::memory_order_release);
        m_readyState.shadowReady.store(true, std::memory_order_release);
        m_readyState.ssaoReady.store(true, std::memory_order_release);
        m_readyState.ssrReady.store(true, std::memory_order_release);
        m_readyState.transparencyReady.store(true, std::memory_order_release);
        m_readyState.tessellationReady.store(true, std::memory_order_release);
        m_readyState.meshShaderReady.store(true, std::memory_order_release);
        m_readyState.cloudReady.store(true, std::memory_order_release);
    }

    bool Renderer::FinalizeCoreInitialization()
    {
        ScopedPerfTimer perfTimer("Renderer::FinalizeCoreInitialization");
        // Everything finalised here -- render target pool, PSO cache, GI, ray tracing -- is built
        // on the D3D12 compatibility surface and is unused when the backend renders its own frame.
        if (UsesNativeBackendFrame(*m_device)) {
            // The ready flags gate boot progress, and every subsystem they name is one this path
            // never builds. Leaving them false stalls startup forever waiting for a raster path
            // that is not coming, so publish them here: nothing is pending.
            MarkRasterSubsystemsReady();
            DebugLog("Renderer::FinalizeCoreInitialization: skipped, backend uses its own frame path.\n");
            return true;
        }

        auto failInit = [](const char* message) -> bool {
            DebugLogDialog(message, L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        };

        const UINT clientW = static_cast<UINT>(m_viewport.Width);
        const UINT clientH = static_cast<UINT>(m_viewport.Height);

        auto allocateSrv = [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) -> bool {
            return m_srvAllocator.Allocate(count, outCpu, outGpu);
        };

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

            CpuTextureRgba8 normalFallback;
            normalFallback.pixels = { 128u, 128u, 255u, 255u };
            normalFallback.width = 1;
            normalFallback.height = 1;

            m_defaultNormalTexture = CreateTextureFromRgba8Data(normalFallback, &batch.commandList, batch.uploadResources);
            if (!m_defaultNormalTexture) {
                return failInit("Renderer::Initialize: normal fallback texture creation failed.\n");
            }

            CpuTextureRgba8 emissiveFallback;
            emissiveFallback.pixels = { 255u, 255u, 255u, 255u };
            emissiveFallback.width = 1;
            emissiveFallback.height = 1;

            m_defaultEmissiveTexture = CreateTextureFromRgba8Data(emissiveFallback, &batch.commandList, batch.uploadResources);
            if (!m_defaultEmissiveTexture) {
                return failInit("Renderer::Initialize: emissive fallback texture creation failed.\n");
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
            // The 8 RayTracing-block subsystem inits themselves moved to deferred tasks
            // (see RegisterDeferredInitTasks); this is the fatal RenderTargetPool setup
            // that used to precede them in the same "Renderer::Initialize.RayTracing" scope.
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing");
            if (!m_renderTargetPool.EnsureRayTracingOutput(*m_device, clientW, clientH)) {
                return failInit("Renderer::Initialize: ray tracing resources initialization failed.\n");
            }
            m_rayTracingStats.hardwareSupported = IsHardwareRayTracingSupported();
        }

        {
            // Wires pointers into the (not-yet-Initialize()'d) RT subsystems; the objects
            // themselves are populated by the deferred tasks below. Safe because these are
            // stable Renderer member addresses, not snapshotted values.
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
                    m_computeFrameFenceValues.assign(frameCount, 0ull);
                    m_computeFrameFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (!m_computeFrameFenceEvent) {
                        DebugLog("Renderer::Initialize: compute frame fence event creation failed.\n");
                        m_computeCmdListReady = false;
                        m_computeFrameFenceValues.clear();
                    } else {
                        DebugLog("Renderer::Initialize: async compute queue initialized.\n");
                    }
                }
            }
            if (!computeOk) {
                m_computeAllocators.clear();
                m_computeFrameFenceValues.clear();
                if (m_computeFrameFenceEvent) {
                    CloseHandle(m_computeFrameFenceEvent);
                    m_computeFrameFenceEvent = nullptr;
                }
            }
        }

        // Per-Node graphics command list slots, opened lazily at genuine cross-queue
        // join points (see RenderGraphExecuteContext::acquireNextGraphicsNode). One
        // outer slot per frame-buffer index; inner vectors start empty and grow on
        // first use within a frame.
        m_graphicsNodeCommandLists.clear();
        m_graphicsNodeCommandLists.resize(GetBackBufferCount());

        // Raster PSOs (shadow, SSAO, SSR, transparency, tessellation, mesh shader, cloud)
        // were all built by RenderPipelineStateCache::Initialize in InitializeCore.
        MarkRasterSubsystemsReady();

        return true;
    }

    void Renderer::RegisterDeferredInitTasks(InitTaskScheduler& scheduler)
    {
        // Every task registered here builds on the D3D12 compatibility surface (SWRT, GI probe
        // grid, fluid, particles, DXR). On a backend that renders its own frame they each fail
        // and report themselves non-fatal, which costs a shader compile per pass and buries the
        // real log lines. Nothing consumes their results on that path, so do not register them.
        if (m_device && UsesNativeBackendFrame(*m_device)) {
            DebugLog("Renderer::RegisterDeferredInitTasks: skipped, backend uses its own frame path.\n");
            return;
        }

        scheduler.AddMainThreadTask("SWRT.RayTracer", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.SWRT");
            if (!m_gpuSoftwareRayTracer.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: GpuSoftwareRayTracer initialization failed.\n");
            }
            ApplySwrtModeSetting();
            m_readyState.swrtReady.store(true, std::memory_order_release);
            return true;
        });

        const auto giTask = scheduler.AddMainThreadTask("GI.ProbeGrid", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.GI");
            if (!m_probeGrid.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: IrradianceProbeGrid initialization failed.\n");
            }
            m_readyState.giReady.store(true, std::memory_order_release);
            return true;
        });

        // DebugProbeGridRenderPass reads the probe grid's GPU resources, hence the
        // dependency on GI.ProbeGrid.
        scheduler.AddMainThreadTask("Debug.ProbeGridPass", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.DebugProbeGrid");
            if (!m_debugVisualization.Initialize(*m_device, m_probeGrid)) {
                DebugLog("Renderer::Initialize: DebugProbeGridRenderPass initialization failed (non-fatal).\n");
            }
            EnsureDebugProbeGridPassInserted();
            return true;
        }, { giTask });

        const auto fluidSimTask = scheduler.AddMainThreadTask("Fluid.Sim", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.FluidSim");
            if (!m_fluidSim.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: FluidHeightfieldSim initialization failed (non-fatal).\n");
            }
            return true;
        });

        scheduler.AddMainThreadTask("Fluid.SurfacePass", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.FluidSurfacePass");
            if (m_fluidSurfaceRenderPass) {
                if (!m_fluidSurfaceRenderPass->Initialize(*m_device)) {
                    DebugLog("Renderer::Initialize: FluidSurfaceRenderPass initialization failed (non-fatal).\n");
                } else {
                    m_fluidSurfaceRenderPass->SetFluidSim(&m_fluidSim);
                }
            }
            // Same insertion point as the debug probe grid; disabled by default (see
            // FluidSurfaceRenderPass::m_enabled), so this is a no-op until the caller opts in.
            EnsureFluidSurfacePassInserted();
            m_readyState.fluidReady.store(true, std::memory_order_release);
            return true;
        }, { fluidSimTask });

        // Smoke gets its own task rather than riding on Fluid.Sim: the two share nothing but a
        // directory, and a failure in one should not stop the other from coming up.
        const auto smokeSimTask = scheduler.AddMainThreadTask("Smoke.Sim", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.SmokeSim");
            if (!m_smokeSim.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: SmokeVolumeSim initialization failed (non-fatal).\n");
            }
            return true;
        });

        scheduler.AddMainThreadTask("Smoke.VolumePass", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.SmokeVolumePass");
            if (!m_smokeVolumeRenderPass) {
                m_smokeVolumeRenderPass = std::make_shared<SmokeVolumeRenderPass>();
            }
            if (!m_smokeVolumeRenderPass->Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: SmokeVolumeRenderPass initialization failed (non-fatal).\n");
            } else {
                m_smokeVolumeRenderPass->SetSmokeSim(&m_smokeSim);
            }
            // Disabled by default, so inserting it here costs nothing until the caller opts in.
            EnsureSmokeVolumePassInserted();
            // The wind fire/smoke particle volume sits right after it (also disabled by default).
            if (!m_particleVolumeRenderPass) {
                m_particleVolumeRenderPass = std::make_shared<ParticleVolumeRenderPass>();
            }
            if (!m_particleVolumeRenderPass->Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: ParticleVolumeRenderPass initialization failed (non-fatal).\n");
            }
            EnsureParticleVolumePassInserted();
            m_readyState.smokeReady.store(true, std::memory_order_release);
            return true;
        }, { smokeSimTask });

        const auto particleSystemTask = scheduler.AddMainThreadTask("Particles.System", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.ParticleSystem");
            if (!m_particleSystem.Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: ParticleSystem initialization failed (non-fatal).\n");
            }
            return true;
        });

        scheduler.AddMainThreadTask("Particles.Pass", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.ParticlePass");
            if (m_particleRenderPass) {
                if (!m_particleRenderPass->Initialize(*m_device)) {
                    DebugLog("Renderer::Initialize: ParticleRenderPass initialization failed (non-fatal).\n");
                } else {
                    m_particleRenderPass->SetParticleSystem(&m_particleSystem);
                }
            }
            EnsureParticlePassInserted();
            m_readyState.particlesReady.store(true, std::memory_order_release);
            return true;
        }, { particleSystemTask });

        scheduler.AddMainThreadTask("Crowd.System", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.CrowdSystem");
            std::string crowdError;
            if (!m_crowdSystem.Initialize(*m_device, crowdError)) {
                DebugLog(("Renderer::Initialize: CrowdSystem initialization failed (non-fatal): " + crowdError + "\n").c_str());
            }
            if (m_crowdRenderPass && !m_crowdRenderPass->Initialize(*m_device)) {
                DebugLog("Renderer::Initialize: CrowdRenderPass initialization failed (non-fatal).\n");
            }
            EnsureCrowdPassInserted();
            return true;
        });

        scheduler.AddMainThreadTask("DXR.RayTracer", [this]() -> bool {
            ScopedPerfTimer stepTimer("Renderer::Initialize.RayTracing.DXR");
            if (m_rayTracingStats.hardwareSupported) {
                if (!m_dxrRayTracer.Initialize(*m_device, m_rayTracingDescriptors)) {
                    DebugLog("Renderer::Initialize: DXR initialization failed. Hardware RT is unavailable.\n");
                }
            }
            m_readyState.dxrReady.store(true, std::memory_order_release);
            return true;
        });
    }

    bool Renderer::Initialize(HWND hWnd, UINT width, UINT height)
    {
        ScopedPerfTimer perfTimer("Renderer::Initialize");
        auto failInit = [](const char* message) -> bool {
            DebugLogDialog(message, L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        };

        if (!InitializeCore(hWnd, width, height)) {
            return false; // InitializeCore already showed the failure dialog.
        }
        if (UsesNativeBackendFrame(*m_device)) {
            return true; // Native backend: InitializeCore already did everything needed.
        }
        if (!InitializeFrameInfrastructure()) {
            return failInit("Renderer::Initialize: Frame context initialization failed.\n");
        }
        if (!FinalizeCoreInitialization()) {
            return false; // FinalizeCoreInitialization already showed the failure dialog.
        }

        InitTaskScheduler scheduler;
        MainThreadTaskPump pump;
        RegisterDeferredInitTasks(scheduler);
        while (!scheduler.AllDone()) {
            scheduler.Pump(pump);
            pump.Execute(1e9);
        }

        m_readyState.allReady.store(true, std::memory_order_release);
        return true;
    }


} // namespace SasamiEngine::Render
