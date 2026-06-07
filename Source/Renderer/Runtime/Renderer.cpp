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
#include <vector>
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
    using Math::Mul4x4;

    namespace
    {
        D3D12_RESOURCE_STATES ToCompatibilityDx12State(RhiResourceState state)
        {
            switch (state) {
            case RhiResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RhiResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RhiResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
            case RhiResourceState::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case RhiResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RhiResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case RhiResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
            case RhiResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
            case RhiResourceState::Common:
            default: return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        D3D_PRIMITIVE_TOPOLOGY ToCompatibilityDx12Topology(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            case RhiPrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case RhiPrimitiveTopology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case RhiPrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RhiPrimitiveTopology::PatchList: return D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            case RhiPrimitiveTopology::TriangleList:
            default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            }
        }

        class D3D12CommandListRhiEncoder final : public IRhiCommandEncoder
        {
        public:
            D3D12CommandListRhiEncoder(IRHIDevice& device, CommandList& commandList)
                : m_device(device)
                , m_commandList(commandList)
            {
            }

            void TransitionResources(const RhiResourceTransitionDesc* transitions, uint32_t count) override
            {
                if (!transitions || count == 0) {
                    return;
                }

                std::vector<ResourceBarrier> barriers;
                barriers.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    Resource* resource = m_device.GetD3D12CompatibilityResource(transitions[i].resource);
                    if (!resource || !resource->IsValid()) {
                        continue;
                    }

                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        resource->Get(),
                        ToCompatibilityDx12State(transitions[i].before),
                        ToCompatibilityDx12State(transitions[i].after),
                        transitions[i].subresource));
                }

                if (!barriers.empty()) {
                    m_commandList.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                }
            }

            void SetViewports(const RhiViewport* viewports, uint32_t count) override
            {
                if (!viewports || count == 0) {
                    return;
                }

                std::vector<Viewport> dxViewports(count);
                for (uint32_t i = 0; i < count; ++i) {
                    dxViewports[i] = {
                        viewports[i].x,
                        viewports[i].y,
                        viewports[i].width,
                        viewports[i].height,
                        viewports[i].minDepth,
                        viewports[i].maxDepth,
                    };
                }
                m_commandList.RSSetViewports(count, dxViewports.data());
            }

            void SetScissors(const RhiRect* scissors, uint32_t count) override
            {
                if (!scissors || count == 0) {
                    return;
                }

                std::vector<Rect> dxRects(count);
                for (uint32_t i = 0; i < count; ++i) {
                    dxRects[i] = { scissors[i].left, scissors[i].top, scissors[i].right, scissors[i].bottom };
                }
                m_commandList.RSSetScissorRects(count, dxRects.data());
            }

            void SetGraphicsPipeline(RhiPipelineHandle handle) override
            {
                if (!handle.IsValid()) return;
                auto* pso = reinterpret_cast<ID3D12PipelineState*>(static_cast<uintptr_t>(handle.id));
                m_commandList.Get()->SetPipelineState(pso);
            }

            void SetComputePipeline(RhiPipelineHandle handle) override
            {
                SetGraphicsPipeline(handle);
            }

            void SetPrimitiveTopology(RhiPrimitiveTopology topology) override
            {
                m_commandList.IASetPrimitiveTopology(ToCompatibilityDx12Topology(topology));
            }

            void Draw(const RhiDrawDesc& desc) override
            {
                m_commandList.DrawInstanced(desc.vertexCount,
                                            desc.instanceCount,
                                            desc.startVertex,
                                            desc.startInstance);
            }

            void DrawIndexed(const RhiDrawIndexedDesc& desc) override
            {
                m_commandList.DrawIndexedInstanced(desc.indexCount,
                                                   desc.instanceCount,
                                                   desc.startIndex,
                                                   desc.baseVertex,
                                                   desc.startInstance);
            }

            void Dispatch(const RhiDispatchDesc& desc) override
            {
                m_commandList.Dispatch(desc.groupCountX, desc.groupCountY, desc.groupCountZ);
            }

            void SetGraphicsPipelineLayout(RhiPipelineLayoutHandle handle) override
            {
                if (!handle.IsValid()) return;
                auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(handle.id));
                m_commandList.Get()->SetGraphicsRootSignature(sig);
            }

            void SetComputePipelineLayout(RhiPipelineLayoutHandle handle) override
            {
                if (!handle.IsValid()) return;
                auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(handle.id));
                m_commandList.Get()->SetComputeRootSignature(sig);
            }

            void SetDescriptorHeap(RhiDescriptorHeapHandle handle) override
            {
                if (!handle.IsValid()) return;
                auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(static_cast<uintptr_t>(handle.id));
                m_commandList.SetDescriptorHeaps(1, &heap);
            }

            void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
            {
                m_commandList.SetGraphicsRootDescriptorTable(slot, { table.ptr });
            }

            void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
            {
                m_commandList.SetComputeRootDescriptorTable(slot, { table.ptr });
            }

            void SetGraphicsConstantBufferView(uint32_t slot, RhiGpuAddress address) override
            {
                m_commandList.SetGraphicsRootConstantBufferView(slot, address);
            }

            void SetComputeConstantBufferView(uint32_t slot, RhiGpuAddress address) override
            {
                m_commandList.SetComputeRootConstantBufferView(slot, address);
            }

            void SetGraphicsShaderResourceView(uint32_t slot, RhiGpuAddress address) override
            {
                m_commandList.SetGraphicsRootShaderResourceView(slot, address);
            }

            void SetComputeShaderResourceView(uint32_t slot, RhiGpuAddress address) override
            {
                m_commandList.SetComputeRootShaderResourceView(slot, address);
            }

            void SetRenderTargets(uint32_t numRtvs,
                                  const RhiCpuDescriptorHandle* rtvs,
                                  const RhiCpuDescriptorHandle* dsv = nullptr) override
            {
                std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dxRtvs(numRtvs);
                for (uint32_t i = 0; i < numRtvs; ++i)
                    dxRtvs[i] = { static_cast<SIZE_T>(rtvs[i].ptr) };
                const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
                D3D12_CPU_DESCRIPTOR_HANDLE dxDsv{};
                if (dsv && dsv->IsValid()) {
                    dxDsv = { static_cast<SIZE_T>(dsv->ptr) };
                    dsvPtr = &dxDsv;
                }
                m_commandList.OMSetRenderTargets(numRtvs,
                                                  numRtvs > 0 ? dxRtvs.data() : nullptr,
                                                  FALSE,
                                                  dsvPtr);
            }

            void ClearRenderTarget(RhiCpuDescriptorHandle rtv, const RhiClearColor& color) override
            {
                const float rgba[4] = { color.r, color.g, color.b, color.a };
                m_commandList.ClearRenderTargetView({ static_cast<SIZE_T>(rtv.ptr) }, rgba, 0, nullptr);
            }

            void ClearDepthStencil(RhiCpuDescriptorHandle dsv, float depth, uint8_t stencil) override
            {
                m_commandList.ClearDepthStencilView({ static_cast<SIZE_T>(dsv.ptr) },
                                                    D3D12_CLEAR_FLAG_DEPTH,
                                                    depth,
                                                    stencil,
                                                    0, nullptr);
            }

            void SetVertexBuffers(uint32_t startSlot, uint32_t count,
                                  const RhiVertexBufferView* views) override
            {
                std::vector<D3D12_VERTEX_BUFFER_VIEW> dxViews(count);
                for (uint32_t i = 0; i < count; ++i) {
                    dxViews[i].BufferLocation = views[i].gpuAddress;
                    dxViews[i].StrideInBytes  = views[i].strideInBytes;
                    dxViews[i].SizeInBytes    = views[i].sizeInBytes;
                }
                m_commandList.IASetVertexBuffers(startSlot, count, dxViews.data());
            }

            void SetIndexBuffer(const RhiIndexBufferView& view) override
            {
                D3D12_INDEX_BUFFER_VIEW ibv{};
                ibv.BufferLocation = view.gpuAddress;
                ibv.SizeInBytes    = view.sizeInBytes;
                ibv.Format         = view.is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                m_commandList.IASetIndexBuffer(&ibv);
            }

        private:
            IRHIDevice& m_device;
            CommandList& m_commandList;
        };

        bool UsesNativeBackendFrame(const IRHIDevice& device)
        {
            return !device.GetCapabilities().supportsFeatureRenderPasses &&
                   device.GetCapabilities().supportsNativeFrame;
        }

        uint32_t ComputeScaledDimension(uint32_t dimension, float scale)
        {
            return std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(dimension) * scale)));
        }

        bool UsesRuntimeAmbientOcclusionMode(RendererEnums::AmbientOcclusionMode mode)
        {
            return mode == RendererEnums::AmbientOcclusionMode::RuntimeAOOnly ||
                   mode == RendererEnums::AmbientOcclusionMode::RayTracedAOOnly ||
                   mode == RendererEnums::AmbientOcclusionMode::Hybrid;
        }

        bool UsesRayTracedRuntimeAmbientOcclusion(RendererEnums::AmbientOcclusionMode mode,
                                                  RendererEnums::RuntimeAmbientOcclusionMethod method)
        {
            return mode != RendererEnums::AmbientOcclusionMode::MaterialOnly &&
                   (method == RendererEnums::RuntimeAmbientOcclusionMethod::RayTraced ||
                    mode == RendererEnums::AmbientOcclusionMode::RayTracedAOOnly);
        }

        bool UsesSsaoRuntimeAmbientOcclusion(RendererEnums::AmbientOcclusionMode mode,
                                             RendererEnums::RuntimeAmbientOcclusionMethod method)
        {
            return UsesRuntimeAmbientOcclusionMode(mode) &&
                   !UsesRayTracedRuntimeAmbientOcclusion(mode, method);
        }

        bool UsesReflectionDebugView(RendererEnums::GBufferDebugView view)
        {
            return view == RendererEnums::GBufferDebugView::ReflectionRadiance ||
                   view == RendererEnums::GBufferDebugView::ReflectionAlpha ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionHitDistance ||
                   view == RendererEnums::GBufferDebugView::SwrtReflectionComposite;
        }
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

    // EnsureEnvironmentTexturesUploaded and BuildSwrtFrameContext
    // are now implemented in EnvironmentManager and SceneSynchronizer respectively.

    RenderPassExecutionPolicy Renderer::BuildRenderPassExecutionPolicy(bool executeOpaqueFamilyPasses,
                                                                       bool executeLightingFamilyPasses,
                                                                       bool useShadowTessPath)
    {
        const SWRTExecutor::PartialBehavior partialBehavior =
            m_swrtExecutor.ResolveBehavior(m_settings.rayTracingPerformancePreset);
        RenderPassExecutionPolicy policy{};
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
        const bool useScreenSpaceReflections =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            !policy.useSoftwareRayTracedReflections &&
            m_settings.rasterScreenSpaceReflectionEnabled &&
            m_sceneColorHistoryValid &&
            m_renderTargetPool.GetTransparentSceneColorCopyTexture().IsValid();
        policy.reflectionMode = policy.useSoftwareRayTracedReflections
            ? 1.0f
            : (useScreenSpaceReflections ? 2.0f : 0.0f);
        policy.softwareRayTracedShadowMapSize =
            policy.useSoftwareRayTracedDirectionalShadow
                ? partialBehavior.shadowMapSize
                : 4096u;
        policy.renderWidth = static_cast<uint32_t>(std::max(0.0f, m_viewport.Width));
        policy.renderHeight = static_cast<uint32_t>(std::max(0.0f, m_viewport.Height));
        policy.iblIntensity = m_settings.iblIntensity;
        policy.gBufferDebugView = m_settings.gBufferDebugView;
        policy.renderPathMode = m_settings.renderPathMode;
        policy.vsmBlurEnabled = m_settings.vsmBlurEnabled;
        m_lightSystem.SetAoMinOcclusion(m_settings.aoMinOcclusion);
        return policy;
    }

    RenderPassFrameInputs Renderer::BuildRenderPassFrameInputs(CommandList* cmdList,
                                                               IRhiCommandEncoder* commandEncoder,
                                                               RendererFrameCoordinator::FrameContext* frame,
                                                               D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                                                               GpuDescriptorHandle defaultAoSrv)
    {
        RenderPassFrameInputs inputs{};
        inputs.execution.cmdList = cmdList;
        inputs.execution.commandEncoder = commandEncoder;
        inputs.execution.pipelineStateCache = &m_pipelineStateCache;
        inputs.execution.srvHeap = m_srvAllocator.GetHeap();
        inputs.execution.viewport = &m_viewport;
        inputs.execution.scissorRect = &m_scissorRect;
        inputs.execution.frameCoordinator = &m_frameCoordinator;
        inputs.execution.frame = frame;

        inputs.lighting.lightSystem = &m_lightSystem;
        inputs.lighting.frameLight = frame ? &frame->light : nullptr;
        inputs.skybox = &m_skybox;
        inputs.camera.pv = m_cameraState.GetPV();
        inputs.camera.invPv = m_cameraState.GetInvPV();
        inputs.camera.pos = m_cameraState.GetPos();
        inputs.camera.right = m_cameraState.GetRight();
        inputs.camera.up = m_cameraState.GetUp();
        inputs.camera.forward = m_cameraState.GetForward();
        inputs.camera.tanHalfFovY = m_cameraState.GetTanHalfFovY();
        inputs.camera.aspectRatio = m_cameraState.GetAspectRatio();
        inputs.camera.mode = m_cameraState.GetCameraMode();
        inputs.shadow.shadowSrv =
            ((m_settings.renderPathMode == RenderPathMode::Raster) &&
             m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled &&
             m_renderTargetPool.GetSWRTShadowTexture().IsValid() &&
             m_swrtExecutor.IsShadowCacheValid())
                ? m_renderTargetPool.GetSWRTShadowSrv()
                : m_lightSystem.GetShadowSrv();
        inputs.shadow.spotShadowSrv = m_lightSystem.GetSpotShadowSrv();
        inputs.shadow.vsmSrv = m_lightSystem.GetVsmSrv();
        inputs.lighting.lightSrvTable = frame ? frame->light.lightSrvTable : GpuDescriptorHandle{};
        inputs.lighting.iblSrvTable = m_skybox.GetIblSrvTable();
        inputs.ao.aoSrv = defaultAoSrv;
        const bool useSwrtReflection =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled &&
            m_renderTargetPool.GetSWRTReflectionTexture().IsValid();
        const bool useScreenSpaceReflection =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            !useSwrtReflection &&
            m_settings.rasterScreenSpaceReflectionEnabled &&
            m_sceneColorHistoryValid &&
            m_renderTargetPool.GetTransparentSceneColorCopyTexture().IsValid();
        inputs.reflectionSrv = useSwrtReflection
            ? m_renderTargetPool.GetSWRTReflectionSrv()
            : (useScreenSpaceReflection ? m_renderTargetPool.GetTransparentSceneColorCopySrv() : m_nullTextureSrv);
        inputs.transmissionSceneColorSrv = m_renderTargetPool.GetTransmissionSceneColorCopyTexture().IsValid()
            ? m_renderTargetPool.GetTransmissionSceneColorCopySrv()
            : m_nullTextureSrv;
        inputs.transparentBackfaceDistanceSrv = m_renderTargetPool.GetTransparentBackfaceDistanceTexture().IsValid()
            ? m_renderTargetPool.GetTransparentBackfaceDistanceSrv()
            : m_nullTextureSrv;
        inputs.transparentOitAccumSrv = m_renderTargetPool.GetTransparentOitAccumTexture().IsValid()
            ? m_renderTargetPool.GetTransparentOitAccumSrv()
            : m_nullTextureSrv;
        inputs.transparentOitRevealageSrv = m_renderTargetPool.GetTransparentOitRevealageTexture().IsValid()
            ? m_renderTargetPool.GetTransparentOitRevealageSrv()
            : m_nullTextureSrv;
        inputs.lighting.lightCbGpu = lightCbGpu;
        inputs.sceneTimeSec = m_sceneTime;

        // GBuffer SRV inputs for deferred/composition passes.
        inputs.gbuffer.albedoSrv   = m_renderTargetPool.GetGBufferAlbedoSrv();
        inputs.gbuffer.materialSrv = m_renderTargetPool.GetGBufferMaterialSrv();
        inputs.gbuffer.emissiveSrv = m_renderTargetPool.GetGBufferEmissiveSrv();

        // Runtime AO resources. SSAO and RTAO share the same AO consumption slot in lighting.
        inputs.gbuffer.depthSrv = m_renderTargetPool.GetDepthSrv();
        inputs.gbuffer.normalSrv = m_renderTargetPool.GetGBufferNormal().IsValid()
            ? m_renderTargetPool.GetGBufferNormalSrv()
            : m_nullTextureSrv;
        inputs.gbuffer.depthResource = m_renderTargetPool.GetDepth().IsValid() ? m_renderTargetPool.GetDepth().Get() : nullptr;
        const bool usesSsaoAo = m_settings.runtimeAoEnabled &&
            UsesSsaoRuntimeAmbientOcclusion(m_settings.ambientOcclusionMode, m_settings.runtimeAoMethod);
        const bool usesRayTracedAo = m_settings.runtimeAoEnabled &&
            UsesRayTracedRuntimeAmbientOcclusion(m_settings.ambientOcclusionMode, m_settings.runtimeAoMethod);
        inputs.ao.ssaoRtv = m_renderTargetPool.GetSSAORtv();
        inputs.ao.ssaoResource =
            (usesSsaoAo && m_renderTargetPool.GetSSAOTexture().IsValid()) ? m_renderTargetPool.GetSSAOTexture().Get() : nullptr;
        inputs.ao.ssaoRawSrv =
            (usesSsaoAo && m_renderTargetPool.GetSSAOTexture().IsValid()) ? m_renderTargetPool.GetSSAOSrv() : m_nullTextureSrv;
        inputs.ao.ssaoBlurRtv = m_renderTargetPool.GetSSAOBlurRtv();
        inputs.ao.ssaoBlurResource =
            (usesSsaoAo && m_renderTargetPool.GetSSAOBlurTexture().IsValid()) ? m_renderTargetPool.GetSSAOBlurTexture().Get() : nullptr;
        switch (m_settings.ambientOcclusionMode) {
        case RendererEnums::AmbientOcclusionMode::MaterialOnly:
            inputs.ao.screenSpaceAoSrv = defaultAoSrv;
            break;
        case RendererEnums::AmbientOcclusionMode::RuntimeAOOnly:
        case RendererEnums::AmbientOcclusionMode::Hybrid:
            if (usesRayTracedAo) {
                inputs.ao.screenSpaceAoSrv =
                    m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                        ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                        : defaultAoSrv;
            } else {
                inputs.ao.screenSpaceAoSrv =
                    m_renderTargetPool.GetSSAOBlurTexture().IsValid() ? m_renderTargetPool.GetSSAOBlurSrv()
                    : m_renderTargetPool.GetSSAOTexture().IsValid() ? m_renderTargetPool.GetSSAOSrv()
                    : defaultAoSrv;
            }
            break;
        case RendererEnums::AmbientOcclusionMode::RayTracedAOOnly:
            inputs.ao.screenSpaceAoSrv =
                m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                    ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                    : defaultAoSrv;
            break;
        default:
            inputs.ao.screenSpaceAoSrv = defaultAoSrv;
            break;
        }

        // Push Runtime AO CB for the SSAO generator. RTAO uses the same tuning fields in SWRTExecutor.
        if (frame && usesSsaoAo) {
            const float ssaoExtra0[4] = {
                m_settings.runtimeAoRadius,
                m_settings.runtimeAoBias,
                m_settings.runtimeAoIntensity,
                m_settings.runtimeAoThickness
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
                static_cast<float>(m_settings.runtimeAoQuality)
            };
            inputs.ao.ssaoCbGpu = m_frameCoordinator.PushCameraCB(
                *frame, m_cameraState.GetPV(), m_cameraState.GetInvPV(), ssaoExtra0, ssaoExtra1, ssaoExtra2);
        }

        return inputs;
    }

    RenderPassExecutionServices Renderer::BuildRenderPassExecutionServices(
        const DrawSceneItemsCallback& drawItems,
        const DrawShadowItemsCallback& drawShadowItems)
    {
        RenderPassExecutionServices services{};
        services.drawOpaqueItems = [drawItems]() { drawItems(false); };
        services.drawTransparentItems = [drawItems]() { drawItems(true); };
        services.drawShadowItems = drawShadowItems;
        services.copySceneColorForTransmission = []() { return true; };
        return services;
    }


    Renderer::Renderer()
        : m_sceneSynchronizer(m_sceneSubmitter,
                              m_cameraState,
                              m_rayTracingScene,
                              m_swrtExecutor,
                              m_drawCommandBuilder,
                              m_deltaTime,
                              m_viewport)
        , m_environmentManager(m_skybox, m_swrtExecutor)
    {
        ScopedPerfTimer perfTimer("Renderer::Renderer");

        m_renderPassBuilderCatalog = RenderPassBuilderCatalog::CreateDefault();

        RenderPassBuildContext buildContext{};
        buildContext.capabilities.supportsFeatureRenderPasses = true;
        buildContext.capabilities.supportsD3D12CompatibilitySurface = true;
        buildContext.capabilities.supportsHardwareRayTracing = true;
        buildContext.capabilities.supportsComputeQueue = true;
        buildContext.capabilities.supportsSwapChain = true;

        auto builtins = m_renderPassBuilderCatalog.BuildBuiltinSequencePasses(buildContext);
        m_shadowRenderPass = std::dynamic_pointer_cast<ShadowRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Shadow)]);
        m_opaqueRenderPass = std::dynamic_pointer_cast<OpaqueRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Opaque)]);
        m_lightingRenderPass = std::dynamic_pointer_cast<LightingRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Lighting)]);
        m_transparentRenderPass = std::dynamic_pointer_cast<TransparentRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Transparent)]);
        m_transparentLightingRenderPass = std::dynamic_pointer_cast<TransparentLightingRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentLighting)]);
        m_transparentBackfaceDistanceRenderPass = std::dynamic_pointer_cast<TransparentBackfaceDistanceRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentBackfaceDistance)]);
        m_transparentCompositeRenderPass = std::dynamic_pointer_cast<TransparentCompositeRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentComposite)]);
        m_transparentSceneColorCopyRenderPass = std::dynamic_pointer_cast<TransparentSceneColorCopyRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::TransparentSceneColorCopy)]);
        m_skyboxRenderPass = std::dynamic_pointer_cast<SkyboxRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::Skybox)]);
        m_postProcessRenderPass = std::dynamic_pointer_cast<PostProcessRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::PostProcess)]);
        m_ssaoRenderPass = std::dynamic_pointer_cast<SSAORenderPass>(
            builtins[static_cast<size_t>(RenderPassType::RuntimeAO)]);
        m_ssaoBlurRenderPass = std::dynamic_pointer_cast<SSAOBlurRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::RuntimeAOBlur)]);
        m_softwareReflectionRenderPass = std::dynamic_pointer_cast<SoftwareReflectionRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::SoftwareReflection)]);
        m_softwareReflectionCompositeRenderPass = std::dynamic_pointer_cast<SoftwareReflectionCompositeRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::SoftwareReflectionComposite)]);
        m_proceduralSkyRenderPass = std::dynamic_pointer_cast<ProceduralSkyRenderPass>(
            builtins[static_cast<size_t>(RenderPassType::ProceduralSky)]);
        m_rayTracingRenderPass = std::dynamic_pointer_cast<RayTracingRenderPass>(
            m_renderPassBuilderCatalog.Build(RenderPassBuilderId::RayTracing, buildContext));
        m_volumetricCloudRenderPass = std::dynamic_pointer_cast<VolumetricCloudRenderPass>(
            m_renderPassBuilderCatalog.Build(RenderPassBuilderId::VolumetricCloud, buildContext));
        m_debugProbeGridRenderPass = std::dynamic_pointer_cast<DebugProbeGridRenderPass>(
            m_renderPassBuilderCatalog.Build(RenderPassBuilderId::DebugProbeGrid, buildContext));

        m_passRegistry.SetBuiltinPasses(builtins);

        m_passRegistry.UseDefaultRenderNodePreset();
    }

    Renderer::~Renderer()
    {
        WaitForGPU();

        m_frameCoordinator.Shutdown(m_lightSystem);

        m_skybox.Shutdown();

        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
    }

    // Renderer::Initialize ↁERendererInitialization.cpp

    // Back buffer management is now delegated to RenderTargetPool.

    void Renderer::WaitForGPU()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
    }

    bool Renderer::IsHardwareRayTracingSupported() const
    {
        return m_device ? m_device->GetCapabilities().supportsHardwareRayTracing : false;
    }

    const RhiBackendCapabilities& Renderer::GetBackendCapabilities() const
    {
        static const RhiBackendCapabilities kNoCapabilities{};
        return m_device ? m_device->GetCapabilities() : kNoCapabilities;
    }

    void Renderer::SetRenderPathMode(RenderPathMode mode)
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

        EnsureVolumetricCloudPassInserted();
    }

    void Renderer::SetVolumetricCloudEnabled(bool e)
    {
        m_settings.volumetricCloudEnabled = e;
        if (m_volumetricCloudRenderPass) {
            m_volumetricCloudRenderPass->SetEnabled(e);
        }
        if (e) {
            EnsureVolumetricCloudPassInserted();
        }
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

        if (UsesNativeBackendFrame(*m_device)) {
            m_device->WaitForGPU();
            m_viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
            m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
            if (!m_device->ResizeBackendSwapChain(width, height)) {
                DebugLog("Renderer::ResizeViewport: native backend swapchain resize failed; viewport state was updated only.\n");
            }
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
        m_sceneColorHistoryValid = false;

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
        m_environmentManager.RefreshEnvironmentAssets();
    }

    void Renderer::SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height)
    {
        m_environmentManager.SetSkyboxHdrEquirectData(std::move(pixels), width, height);
    }

    void Renderer::SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height)
    {
        m_environmentManager.SetSkyboxLdrEquirectData(std::move(pixels), width, height);
    }

    void Renderer::SetSkyboxLdrCubemapFacesData(std::vector<std::vector<uint8_t>> facePixels, UINT width, UINT height)
    {
        m_environmentManager.SetSkyboxLdrCubemapFacesData(std::move(facePixels), width, height);
    }

    void Renderer::SetSkyboxLoadFormat(SkyboxLoadFormat format)
    {
        m_environmentManager.SetSkyboxLoadFormat(format);
    }

    Renderer::NodeHandle Renderer::AddNode(const std::shared_ptr<IRenderNode>& renderNode)
    {
        return m_passRegistry.AddNode(renderNode);
    }

    void Renderer::SetRenderNodePreset(std::shared_ptr<IRenderNode> renderNode)
    {
        m_passRegistry.SetRenderNodePreset(std::move(renderNode));
    }

    void Renderer::UseDefaultRenderNodePreset()
    {
        m_passRegistry.UseDefaultRenderNodePreset();
        EnsureVolumetricCloudPassInserted();
    }

    Renderer::PassHandle Renderer::AddPass(const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.AddPass(renderPass);
    }

    Renderer::PassHandle Renderer::AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.AddPassBefore(targetTag, renderPass);
    }

    Renderer::PassHandle Renderer::AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.AddPassAfter(targetTag, renderPass);
    }

    bool Renderer::ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.ReplacePass(targetTag, renderPass);
    }

    bool Renderer::HasRenderPass(std::string_view tag) const
    {
        const auto& passes = m_passRegistry.GetPasses();
        return std::any_of(passes.begin(), passes.end(),
                           [tag](const std::shared_ptr<IRenderPass>& pass) {
                               return pass && pass->Tag() == tag;
                           });
    }

    void Renderer::EnsureVolumetricCloudPassInserted()
    {
        if (!m_volumetricCloudRenderPass ||
            !m_settings.volumetricCloudEnabled ||
            HasRenderPass("VolumetricCloud")) {
            return;
        }

        m_volumetricCloudRenderPass->SetEnabled(true);
        if (AddPassAfter("Skybox", m_volumetricCloudRenderPass).IsValid()) return;
        if (AddPassAfter("ProceduralSky", m_volumetricCloudRenderPass).IsValid()) return;
        if (AddPassAfter("Lighting", m_volumetricCloudRenderPass).IsValid()) return;
        AddPass(m_volumetricCloudRenderPass);
    }

    bool Renderer::AddPhaseCompletionNode(std::string_view phaseTag,
                                          std::string_view nodeName,
                                          const PhaseCompletionCallback& execute,
                                          PhaseCompletionMode mode,
                                          const RenderPassRequirements& requirements)
    {
        return m_passRegistry.AddPhaseCompletionNode(phaseTag, nodeName, execute, mode, requirements);
    }

    void Renderer::ClearPhaseCompletionNodes()
    {
        m_passRegistry.ClearPhaseCompletionNodes();
    }

    void Renderer::ClearPasses()
    {
        WaitForGPU();
        m_passRegistry.ClearPasses();
    }

    void Renderer::ResetAndRebakeGI()
    {
        m_giBakeClearPending = true;  // ClearProbeBuffer on next frame (device+cmdList available)
        m_giBakeRequested    = true;
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

    void Renderer::SetRenderPassSequence(const std::vector<RenderPassType>& sequence)
    {
        m_passRegistry.SetRenderPassSequence(sequence);

        // Re-insert non-builtin nodes that were previously added via AddPassBefore/AddPassAfter.
        // SetRenderPassSequence calls ClearPasses() internally, which removes them.
        // Keep the probe grid after Lighting/Skybox so deferred PBR has already produced SceneColor.
        if (m_debugProbeGridRenderPass && m_debugProbeGridRenderPass->IsInitialized()) {
            if (!AddPassAfter("Skybox", m_debugProbeGridRenderPass).IsValid()) {
                if (!AddPassAfter("Lighting", m_debugProbeGridRenderPass).IsValid()) {
                    AddPass(m_debugProbeGridRenderPass);
                }
            }
        }
        EnsureVolumetricCloudPassInserted();
    }

    void Renderer::ReinsertDebugProbeGrid()
    {
        if (!m_debugProbeGridRenderPass || !m_debugProbeGridRenderPass->IsInitialized()) return;
        if (AddPassAfter("Skybox", m_debugProbeGridRenderPass).IsValid()) return;
        if (AddPassAfter("Lighting", m_debugProbeGridRenderPass).IsValid()) return;
        AddPass(m_debugProbeGridRenderPass);
    }

    void Renderer::UpdateCameraCB(const RenderCameraProxy* camera)
    {
        m_sceneSynchronizer.UpdateCameraCB(camera);
    }

    void Renderer::SubmitRenderProxies(std::vector<RenderProxy>&& proxies)
    {
        m_sceneSynchronizer.SubmitRenderProxies(std::move(proxies));
    }

    void Renderer::SubmitSkinnedRenderProxies(std::vector<SkinnedRenderProxy>&& proxies)
    {
        const UINT backIndex = m_device ? m_device->GetSwapChain()->GetCurrentBackBufferIndex() : 0;
        auto* frame = m_frameCoordinator.GetFrameContext(backIndex);
        if (!frame) return;
        m_sceneSubmitter.SubmitSkinnedRenderProxies(std::move(proxies), m_frameCoordinator, *frame);
    }

    void Renderer::ClearSubmittedRenderProxies()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
        m_sceneSynchronizer.ClearSubmittedRenderProxies();
        m_sceneSubmitter.ClearSkinnedRenderProxies();
    }

    void Renderer::ClearRenderObjects()
    {
        if (m_device) {
            m_device->WaitForGPU();
        }
        m_sceneSynchronizer.ClearRenderObjects();
        m_sceneSubmitter.ClearSkinnedRenderProxies();
    }
}
