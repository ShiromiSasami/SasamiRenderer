// Renderer_Render.cpp
// Renderer::Render and related frame execution functions.
#include "Renderer/Core/Renderer.h"
#include "Renderer/Core/SceneSynchronizer.h"
#include "Renderer/Core/EnvironmentManager.h"

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
#include "Renderer/Passes/RenderNodeSetupContext.h"
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

    void Renderer::Render(const OverlayRenderCallback& overlay)
    {
        if (!m_device) {
            return;
        }

        if (UsesNativeBackendFrame(*m_device)) {
            if (m_deltaTime > 0.0f) {
                m_sceneTime += m_deltaTime;
            }
            RhiBackendFrameDesc frameDesc{};
            frameDesc.clearColor = { 0.02f, 0.025f, 0.03f, 1.0f };
            frameDesc.present = true;
            (void)overlay;
            m_device->ExecuteBackendFrame(frameDesc);
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

        m_environmentManager.EnsureTexturesUploaded(cmdList);

        m_renderGraph.Clear();
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        if (!backBuffer || !m_renderTargetPool.GetDepth().IsValid() || !m_renderTargetPool.GetDepthDsv().ptr) {
            return;
        }
        const UINT sceneColorW = static_cast<UINT>(m_viewport.Width);
        const UINT sceneColorH = static_cast<UINT>(m_viewport.Height);
        if (!m_renderTargetPool.EnsureSceneColor(*m_device, sceneColorW, sceneColorH) ||
            !m_renderTargetPool.GetSceneColorTexture().IsValid()) {
            return;
        }
        if (!m_renderTargetPool.EnsureTransmissionSceneColorCopy(*m_device, sceneColorW, sceneColorH)) {
            return;
        }
        if (!m_renderTargetPool.EnsureTransparentBackfaceDistance(*m_device, sceneColorW, sceneColorH)) {
            return;
        }
        if (!m_renderTargetPool.EnsureTransparentOit(*m_device, sceneColorW, sceneColorH)) {
            return;
        }

        ExternalRenderGraphResourceDesc sceneColorDesc{};
        sceneColorDesc.resource = m_renderTargetPool.GetSceneColorTexture().Get();
        sceneColorDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sceneColorDesc.finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sceneColorDesc.transitionToFinalState = true;
        sceneColorDesc.rtv = m_renderTargetPool.GetSceneColorRtv();
        sceneColorDesc.hasRtv = true;
        sceneColorDesc.clearColorOnFirstUse = true;
        sceneColorDesc.clearColor[0] = 0.2f;
        sceneColorDesc.clearColor[1] = 0.2f;
        sceneColorDesc.clearColor[2] = 0.2f;
        sceneColorDesc.clearColor[3] = 1.0f;
        m_renderGraph.ImportExternalResource("SceneColor", sceneColorDesc);

        ExternalRenderGraphResourceDesc backBufferDesc{};
        backBufferDesc.resource = backBuffer->Get();
        backBufferDesc.initialState = D3D12_RESOURCE_STATE_PRESENT;
        backBufferDesc.finalState = D3D12_RESOURCE_STATE_PRESENT;
        backBufferDesc.transitionToFinalState = true;
        backBufferDesc.rtv = m_renderTargetPool.GetBackBufferRtv(backIndex);
        backBufferDesc.hasRtv = true;
        backBufferDesc.clearColorOnFirstUse = false;
        m_renderGraph.ImportExternalResource("BackBuffer", backBufferDesc);

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

        ExternalRenderGraphResourceDesc transparentBackfaceDistanceDesc{};
        transparentBackfaceDistanceDesc.resource = m_renderTargetPool.GetTransparentBackfaceDistanceTexture().Get();
        transparentBackfaceDistanceDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transparentBackfaceDistanceDesc.finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transparentBackfaceDistanceDesc.transitionToFinalState = true;
        transparentBackfaceDistanceDesc.rtv = m_renderTargetPool.GetTransparentBackfaceDistanceRtv();
        transparentBackfaceDistanceDesc.hasRtv = true;
        transparentBackfaceDistanceDesc.gpuSrv = m_renderTargetPool.GetTransparentBackfaceDistanceSrv();
        transparentBackfaceDistanceDesc.hasSrv = true;
        transparentBackfaceDistanceDesc.clearColorOnFirstUse = true;
        m_renderGraph.ImportExternalResource("TransparentBackfaceDistance", transparentBackfaceDistanceDesc);

        ExternalRenderGraphResourceDesc transparentOitAccumDesc{};
        transparentOitAccumDesc.resource = m_renderTargetPool.GetTransparentOitAccumTexture().Get();
        transparentOitAccumDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transparentOitAccumDesc.finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transparentOitAccumDesc.transitionToFinalState = true;
        transparentOitAccumDesc.rtv = m_renderTargetPool.GetTransparentOitAccumRtv();
        transparentOitAccumDesc.hasRtv = true;
        transparentOitAccumDesc.gpuSrv = m_renderTargetPool.GetTransparentOitAccumSrv();
        transparentOitAccumDesc.hasSrv = true;
        transparentOitAccumDesc.clearColorOnFirstUse = true;
        m_renderGraph.ImportExternalResource("TransparentOitAccum", transparentOitAccumDesc);

        ExternalRenderGraphResourceDesc transparentOitRevealageDesc{};
        transparentOitRevealageDesc.resource = m_renderTargetPool.GetTransparentOitRevealageTexture().Get();
        transparentOitRevealageDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transparentOitRevealageDesc.finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transparentOitRevealageDesc.transitionToFinalState = true;
        transparentOitRevealageDesc.rtv = m_renderTargetPool.GetTransparentOitRevealageRtv();
        transparentOitRevealageDesc.hasRtv = true;
        transparentOitRevealageDesc.gpuSrv = m_renderTargetPool.GetTransparentOitRevealageSrv();
        transparentOitRevealageDesc.hasSrv = true;
        transparentOitRevealageDesc.clearColorOnFirstUse = true;
        transparentOitRevealageDesc.clearColor[0] = 1.0f;
        transparentOitRevealageDesc.clearColor[1] = 1.0f;
        transparentOitRevealageDesc.clearColor[2] = 1.0f;
        transparentOitRevealageDesc.clearColor[3] = 1.0f;
        m_renderGraph.ImportExternalResource("TransparentOitRevealage", transparentOitRevealageDesc);

        // Import GBuffer resources so LightingRenderNode can bind them as MRTs.
        const UINT gbufferW = static_cast<UINT>(m_viewport.Width);
        const UINT gbufferH = static_cast<UINT>(m_viewport.Height);
        m_renderTargetPool.EnsureGBuffer(*m_device, gbufferW, gbufferH);
        if (m_renderTargetPool.GetGBufferAlbedo().IsValid()) {
            const auto importGBuffer = [&](const char* name, Resource& res, CpuDescriptorHandle rtv, GpuDescriptorHandle srv) {
                ExternalRenderGraphResourceDesc d{};
                d.resource = res.Get();
                d.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                d.finalState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                d.transitionToFinalState = true;
                d.rtv = rtv;
                d.hasRtv = true;
                d.gpuSrv = srv;
                d.hasSrv = true;
                d.clearColorOnFirstUse = true;
                // clearColor defaults to 0,0,0,0
                m_renderGraph.ImportExternalResource(name, d);
            };
            importGBuffer("GBufferAlbedo",   m_renderTargetPool.GetGBufferAlbedo(),   m_renderTargetPool.GetGBufferAlbedoRtv(),   m_renderTargetPool.GetGBufferAlbedoSrv());
            importGBuffer("GBufferNormal",   m_renderTargetPool.GetGBufferNormal(),   m_renderTargetPool.GetGBufferNormalRtv(),   m_renderTargetPool.GetGBufferNormalSrv());
            importGBuffer("GBufferMaterial", m_renderTargetPool.GetGBufferMaterial(), m_renderTargetPool.GetGBufferMaterialRtv(), m_renderTargetPool.GetGBufferMaterialSrv());
            importGBuffer("GBufferEmissive", m_renderTargetPool.GetGBufferEmissive(), m_renderTargetPool.GetGBufferEmissiveRtv(), m_renderTargetPool.GetGBufferEmissiveSrv());
        }

        D3D12CommandListRhiEncoder graphicsCommandEncoder(*m_device, *cmdList);

        auto drawItems = [this, &graphicsCommandEncoder, frame](bool drawTransparent) {
            auto* enc = &graphicsCommandEncoder;
            // Bind runtime-generated AO texture at slot 8 (t9 in shader) once per batch.
            GpuDescriptorHandle runtimeAoSrv = m_nullTextureSrv;
            const bool usesRayTracedAo = m_settings.runtimeAoEnabled &&
                UsesRayTracedRuntimeAmbientOcclusion(m_settings.ambientOcclusionMode, m_settings.runtimeAoMethod);
            switch (m_settings.ambientOcclusionMode) {
            case RendererEnums::AmbientOcclusionMode::MaterialOnly:
                runtimeAoSrv = m_nullTextureSrv;
                break;
            case RendererEnums::AmbientOcclusionMode::RuntimeAOOnly:
            case RendererEnums::AmbientOcclusionMode::Hybrid:
            {
                // Fallback to white (AO=1) rather than null (AO=0) when runtime AO is not ready.
                const GpuDescriptorHandle runtimeAoFallback = m_defaultOcclusionTexture
                    ? m_defaultOcclusionTexture->srv : m_nullTextureSrv;
                if (usesRayTracedAo) {
                    runtimeAoSrv =
                        m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                            ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                            : runtimeAoFallback;
                } else if (m_settings.gBufferDebugView == RendererEnums::GBufferDebugView::RuntimeAmbientOcclusionRaw) {
                    runtimeAoSrv = m_renderTargetPool.GetSSAOTexture().IsValid()
                        ? m_renderTargetPool.GetSSAOSrv()
                        : runtimeAoFallback;
                } else {
                    runtimeAoSrv =
                        m_renderTargetPool.GetSSAOBlurTexture().IsValid() ? m_renderTargetPool.GetSSAOBlurSrv()
                        : m_renderTargetPool.GetSSAOTexture().IsValid() ? m_renderTargetPool.GetSSAOSrv()
                        : runtimeAoFallback;
                }
                break;
            }
            case RendererEnums::AmbientOcclusionMode::RayTracedAOOnly:
                runtimeAoSrv =
                    m_renderTargetPool.GetSWRTAmbientOcclusionTexture().IsValid()
                        ? m_renderTargetPool.GetSWRTAmbientOcclusionSrv()
                        : m_nullTextureSrv;
                break;
            default:
                break;
            }
            enc->SetGraphicsDescriptorTable(8, { runtimeAoSrv.ptr });

            // Bind GI probe grid CB (b2) and probe SH data (t10) as inline root descriptors.
            // These are always bound so PBR_PS can read g_giEnabled to decide whether to use probes.
            if (m_probeGrid.IsInitialized()) {
                const RhiGpuAddress probeCbGpu = m_probeGrid.GetProbeGridCbGpuAddress();
                const RhiGpuAddress probeVA    = m_probeGrid.GetProbeDataGpuVA();
                if (probeCbGpu != 0) enc->SetGraphicsConstantBufferView(9, probeCbGpu);
                if (probeVA    != 0) enc->SetGraphicsShaderResourceView(10, probeVA);
            }

            std::vector<const SceneSubmitter::DrawItem*> drawList;
            drawList.reserve(m_sceneSubmitter.GetDrawItems().size());
            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.transparent == drawTransparent) {
                    drawList.push_back(&item);
                }
            }
            if (drawTransparent) {
                const float* cameraPos = m_cameraState.GetPos();
                std::sort(drawList.begin(), drawList.end(),
                    [cameraPos](const SceneSubmitter::DrawItem* lhs, const SceneSubmitter::DrawItem* rhs) {
                        const auto distanceSq = [cameraPos](const SceneSubmitter::DrawItem* item) {
                            const float dx = item->model[12] - cameraPos[0];
                            const float dy = item->model[13] - cameraPos[1];
                            const float dz = item->model[14] - cameraPos[2];
                            return dx * dx + dy * dy + dz * dz;
                        };
                        return distanceSq(lhs) > distanceSq(rhs);
                    });
            }

            for (const SceneSubmitter::DrawItem* drawItem : drawList) {
                const auto& item = *drawItem;

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
                const RendererEnums::AmbientOcclusionMode effectiveAoMode =
                    m_settings.runtimeAoEnabled
                        ? m_settings.ambientOcclusionMode
                        : RendererEnums::AmbientOcclusionMode::MaterialOnly;
                const float extra2[4] = {
                    item.material.metallic,
                    item.material.occlusionStrength,
                    static_cast<float>(effectiveAoMode),
                    item.usesMetallicRoughnessTexture ? 1.0f : 0.0f,
                };
                const float extra3[4] = {
                    item.material.specularColor[0],
                    item.material.specularColor[1],
                    item.material.specularColor[2],
                    static_cast<float>(static_cast<uint32_t>(item.material.workflow)),
                };
                const float extra4[4] = {
                    item.material.reflectionStrength,
                    item.material.transmission,
                    item.material.ior,
                    item.material.thickness,
                };
                const float extra5[4] = {
                    item.material.attenuationColor[0],
                    item.material.attenuationColor[1],
                    item.material.attenuationColor[2],
                    item.material.attenuationDistance,
                };
                const float extra6[4] = {
                    item.material.transparentShellStrength,
                    0.0f,
                    0.0f,
                    0.0f,
                };
                float objMVP[16];
                Mul4x4(item.model, m_cameraState.GetPV(), objMVP);
                const RhiGpuAddress cameraCbGpu =
                    m_frameCoordinator.PushCameraCB(*frame, objMVP, item.model, extra0, extra1, extra2, extra3, extra4, extra5, extra6);
                if (cameraCbGpu != 0) {
                    enc->SetGraphicsConstantBufferView(2, cameraCbGpu);
                }

                if (item.texture) {
                    enc->SetGraphicsDescriptorTable(0, { item.texture->srv.ptr });
                } else if (m_defaultAlbedoTexture) {
                    enc->SetGraphicsDescriptorTable(0, { m_defaultAlbedoTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(0, { m_nullTextureSrv.ptr });
                }

                if (item.occlusionTexture) {
                    enc->SetGraphicsDescriptorTable(6, { item.occlusionTexture->srv.ptr });
                } else if (m_defaultOcclusionTexture) {
                    enc->SetGraphicsDescriptorTable(6, { m_defaultOcclusionTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(6, { m_nullTextureSrv.ptr });
                }

                m_meshBuffer.Bind(enc, item.meshIndex);
                const auto& items = m_meshBuffer.Items();
                if (item.meshIndex < items.size()) {
                    const auto& it = items[item.meshIndex];
                    if (it.indexCount > 0) {
                        enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
                    } else if (it.vertexCount > 0) {
                        enc->Draw({ it.vertexCount, 1, 0, 0 });
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
            m_settings.runtimeAoEnabled &&
            UsesRayTracedRuntimeAmbientOcclusion(m_settings.ambientOcclusionMode,
                                                 m_settings.runtimeAoMethod);
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
            const float reflectionScale = UsesReflectionDebugView(m_settings.gBufferDebugView)
                ? 1.0f
                : partialBehavior.reflectionResolutionScale;
            const uint32_t reflectionWidth  = ComputeScaledDimension(
                static_cast<uint32_t>(m_viewport.Width),  reflectionScale);
            const uint32_t reflectionHeight = ComputeScaledDimension(
                static_cast<uint32_t>(m_viewport.Height), reflectionScale);
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
                DebugLog("Renderer::Render: failed to prepare ray-traced runtime AO resources. Falling back to default AO.\n");
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

        auto drawShadowItems = [this, &graphicsCommandEncoder, frame](const LightSystem::ShadowPassContext& context) {
            auto* enc = &graphicsCommandEncoder;
            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.transparent) {
                    continue;
                }
                float objLightMVP[16];
                Mul4x4(item.model, context.lightViewProjection, objLightMVP);
                const RhiGpuAddress cameraCbGpu =
                    m_frameCoordinator.PushCameraCB(*frame, objLightMVP, item.model);
                if (cameraCbGpu != 0) {
                    enc->SetGraphicsConstantBufferView(2, cameraCbGpu);
                }

                m_meshBuffer.Bind(enc, item.meshIndex);
                const auto& items = m_meshBuffer.Items();
                if (item.meshIndex < items.size()) {
                    const auto& it = items[item.meshIndex];
                    if (it.indexCount > 0) {
                        enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
                    } else if (it.vertexCount > 0) {
                        enc->Draw({ it.vertexCount, 1, 0, 0 });
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
            &graphicsCommandEncoder,
            frame,
            lightCbGpu,
            defaultAoSrv);

        // Prepare compute command list if async compute is available.
        CommandList* computeCmdList = nullptr;
        std::unique_ptr<D3D12CommandListRhiEncoder> computeCommandEncoder;
        RenderNodeFrameInputs computeFrameInputs{};
        if (m_computeCmdListReady && !m_computeAllocators.empty()) {
            CommandAllocator& computeAlloc = m_computeAllocators[backIndex % m_computeAllocators.size()];
            computeAlloc.Reset();
            if (SUCCEEDED(m_computeCmdList.Reset(computeAlloc, nullptr))) {
                computeCmdList = &m_computeCmdList;
                computeCommandEncoder = std::make_unique<D3D12CommandListRhiEncoder>(*m_device, *computeCmdList);
                computeFrameInputs = frameInputs;
                computeFrameInputs.execution.cmdList = computeCmdList;
                computeFrameInputs.execution.commandEncoder = computeCommandEncoder.get();
                computeFrameInputs.execution.computeCommandEncoder = computeCommandEncoder.get();
                frameInputs.execution.computeCmdList = computeCmdList;
                frameInputs.execution.computeCommandEncoder = computeCommandEncoder.get();
            }
        }

        auto drawSkinnedItems = [this, &graphicsCommandEncoder, frame](bool drawTransparent) {
            auto* enc = &graphicsCommandEncoder;
            std::vector<const SceneSubmitter::SkinnedDrawItem*> drawList;
            drawList.reserve(m_sceneSubmitter.GetSkinnedDrawItems().size());
            for (const auto& item : m_sceneSubmitter.GetSkinnedDrawItems()) {
                if (item.transparent == drawTransparent) {
                    drawList.push_back(&item);
                }
            }
            if (drawTransparent) {
                const float* cameraPos = m_cameraState.GetPos();
                std::sort(drawList.begin(), drawList.end(),
                    [cameraPos](const SceneSubmitter::SkinnedDrawItem* lhs, const SceneSubmitter::SkinnedDrawItem* rhs) {
                        const auto distanceSq = [cameraPos](const SceneSubmitter::SkinnedDrawItem* item) {
                            const float dx = item->model[12] - cameraPos[0];
                            const float dy = item->model[13] - cameraPos[1];
                            const float dz = item->model[14] - cameraPos[2];
                            return dx * dx + dy * dy + dz * dz;
                        };
                        return distanceSq(lhs) > distanceSq(rhs);
                    });
            }

            for (const SceneSubmitter::SkinnedDrawItem* drawItem : drawList) {
                const auto& item = *drawItem;

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
                const RendererEnums::AmbientOcclusionMode effectiveAoMode =
                    m_settings.runtimeAoEnabled
                        ? m_settings.ambientOcclusionMode
                        : RendererEnums::AmbientOcclusionMode::MaterialOnly;
                const float extra2[4] = {
                    item.material.metallic,
                    item.material.occlusionStrength,
                    static_cast<float>(effectiveAoMode),
                    0.0f,
                };
                const float extra3[4] = {
                    item.material.specularColor[0],
                    item.material.specularColor[1],
                    item.material.specularColor[2],
                    static_cast<float>(static_cast<uint32_t>(item.material.workflow)),
                };
                const float extra4[4] = {
                    item.material.reflectionStrength,
                    item.material.transmission,
                    item.material.ior,
                    item.material.thickness,
                };
                const float extra5[4] = {
                    item.material.attenuationColor[0],
                    item.material.attenuationColor[1],
                    item.material.attenuationColor[2],
                    item.material.attenuationDistance,
                };
                const float extra6[4] = {
                    item.material.transparentShellStrength,
                    0.0f,
                    0.0f,
                    0.0f,
                };

                float objMVP[16];
                Mul4x4(item.model, m_cameraState.GetPV(), objMVP);
                const RhiGpuAddress cameraCbGpu =
                    m_frameCoordinator.PushCameraCB(*frame, objMVP, item.model, extra0, extra1, extra2, extra3, extra4, extra5, extra6);
                if (cameraCbGpu != 0) {
                    enc->SetGraphicsConstantBufferView(2, cameraCbGpu);
                }

                if (item.boneMatricesCbGpu != 0) {
                    enc->SetGraphicsConstantBufferView(15, item.boneMatricesCbGpu);
                }

                if (item.texture) {
                    enc->SetGraphicsDescriptorTable(0, { item.texture->srv.ptr });
                } else if (m_defaultAlbedoTexture) {
                    enc->SetGraphicsDescriptorTable(0, { m_defaultAlbedoTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(0, { m_nullTextureSrv.ptr });
                }

                if (item.occlusionTexture) {
                    enc->SetGraphicsDescriptorTable(6, { item.occlusionTexture->srv.ptr });
                } else if (m_defaultOcclusionTexture) {
                    enc->SetGraphicsDescriptorTable(6, { m_defaultOcclusionTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(6, { m_nullTextureSrv.ptr });
                }

                m_skinnedMeshBuffer.Bind(enc, item.meshIndex);
                const auto& gpuItems = m_skinnedMeshBuffer.Items();
                if (item.meshIndex < gpuItems.size()) {
                    const auto& it = gpuItems[item.meshIndex];
                    if (it.indexCount > 0) {
                        enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
                    } else if (it.vertexCount > 0) {
                        enc->Draw({ it.vertexCount, 1, 0, 0 });
                    }
                }
            }
        };

        RenderNodeExecutionServices executionServices = BuildRenderNodeExecutionServices(
            drawItems,
            drawShadowItems);
        if (!m_sceneSubmitter.GetSkinnedDrawItems().empty()) {
            executionServices.drawSkinnedOpaqueItems      = [drawSkinnedItems]() { drawSkinnedItems(false); };
            executionServices.drawSkinnedTransparentItems = [drawSkinnedItems]() { drawSkinnedItems(true); };
        }
        executionServices.toneMapSceneColor = [this, cmdList, backIndex]() {
            return ToneMapSceneColor(cmdList, backIndex);
        };
        executionServices.copySceneColorForTransmission = [this, cmdList]() {
            return CopySceneColorForTransmission(cmdList);
        };
        executionServices.executeSoftwareDirectionalShadow = [this, cmdList, partialBehavior](const LightSystem::ShadowPassContext& shadowContext) {
            const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteDirectionalShadow(cmdList, shadowContext, ctx, partialBehavior, m_settings, m_rayTracingStats);
        };
        executionServices.executeSoftwareReflections = [this, cmdList, partialBehavior]() {
            const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteReflections(cmdList, ctx, partialBehavior, m_settings, m_rayTracingStats);
        };
        executionServices.executeRayTracing = [this, cmdList, backIndex]() {
            const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteHardware(cmdList, backIndex, ctx, m_settings, m_rayTracingStats);
        };

        m_passRegistry.ClearPhaseCompletionNodes();
        if (useSoftwareRayTracedAmbientOcclusion) {
            m_passRegistry.AddPhaseCompletionNode(
                "Scene",
                "SwrtAmbientOcclusion",
                [this, cmdList](const RenderNodeContextView&) -> bool {
                    const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
                    return m_swrtExecutor.ExecuteAmbientOcclusion(cmdList, ctx, m_settings, m_rayTracingStats);
                },
                PhaseCompletionMode::Deterministic,
                {});
        }
        if (useSoftwareRayTracedReflections) {
            if (UsesReflectionDebugView(m_settings.gBufferDebugView) &&
                m_settings.gBufferDebugView != RendererEnums::GBufferDebugView::SwrtReflectionComposite) {
                m_passRegistry.AddPhaseCompletionNode(
                    "Scene",
                    "SwrtReflections",
                    [this, cmdList, partialBehavior](const RenderNodeContextView&) -> bool {
                        const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
                        return m_swrtExecutor.ExecuteReflections(cmdList, ctx, partialBehavior, m_settings, m_rayTracingStats);
                    },
                    PhaseCompletionMode::Deterministic,
                    {});
            }
            if (m_settings.gBufferDebugView == RendererEnums::GBufferDebugView::FinalLit ||
                m_settings.gBufferDebugView == RendererEnums::GBufferDebugView::SwrtReflectionComposite) {
                executionServices.compositeSoftwareReflections =
                    [this, cmdList, backIndex, lightCbGpu]() -> bool {
                        return CompositeSoftwareReflections(cmdList, backIndex, lightCbGpu);
                    };
            }
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

        CaptureSceneColorHistory(cmdList, backIndex);

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


} // namespace SasamiRenderer
