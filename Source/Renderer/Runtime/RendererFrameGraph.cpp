// RendererFrameGraph.cpp
// Renderer::Render and related frame execution functions.
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Culling/Frustum.h"

#include <cstdio>
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
#include "Foundation/Profiling/Profiler.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/Passes/RayMarch/RayMarchRenderPass.h"
#include "Renderer/Passes/PostProcess/FxaaRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "d3dx12.h"

using namespace std;


namespace SasamiRenderer
{
    namespace
    {
        // Periodic culling report. Sampled rather than per-frame because these loops run several
        // times a frame (one per cascade, plus opaque and transparent) and a line each would bury
        // the rest of the log. The point is to make the effect of culling measurable: before this
        // existed the shadow pass drew meshCount * cascadeCount every frame with no way to see it.
        // The sample counter is per-label. A single shared counter cannot be used here: the
        // call sites have different call rates (four cascades per frame vs one opaque pass),
        // so one counter makes each label's samples land on an arbitrary subset of frames and
        // cascades. Reading those lines as a per-cascade breakdown of one frame silently
        // produces a wrong draw-call total -- they are unrelated samples from unrelated frames.
        void LogCullingStats(const char* label, size_t considered, size_t drawn)
        {
            static std::unordered_map<std::string, unsigned int> s_sampleCounters;
            constexpr unsigned int kSampleInterval = 240u;
            if ((s_sampleCounters[label]++ % kSampleInterval) != 0u) {
                return;
            }
            char line[160];
            std::snprintf(line, sizeof(line), "[Culling] %s: drawn=%zu / considered=%zu (culled %zu)\n",
                          label, drawn, considered, considered - drawn);
            DebugLog(line);
        }
    }

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

        std::string FormatGIBvhMissingMask(uint32_t mask)
        {
            if (mask == 0u) {
                return "none";
            }

            std::string result;
            auto append = [&result](const char* name) {
                if (!result.empty()) {
                    result += ", ";
                }
                result += name;
            };

            if ((mask & Renderer::GI_BVH_MISSING_SWRT_NOT_INITIALIZED) != 0u) append("SWRT not initialized");
            if ((mask & Renderer::GI_BVH_MISSING_BVH_NODES) != 0u) append("bvhNodes");
            if ((mask & Renderer::GI_BVH_MISSING_TRIANGLES) != 0u) append("triangles");
            if ((mask & Renderer::GI_BVH_MISSING_MESH_INFO) != 0u) append("meshInfo");
            if ((mask & Renderer::GI_BVH_MISSING_INSTANCES) != 0u) append("instances");
            if ((mask & Renderer::GI_BVH_MISSING_TLAS_NODES) != 0u) append("tlasNodes");
            if ((mask & Renderer::GI_BVH_MISSING_MATERIALS) != 0u) append("materials");
            return result;
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
#if defined(_DEBUG)
                DebugIncrementDrawCount();
#endif
                m_commandList.DrawInstanced(desc.vertexCount,
                                            desc.instanceCount,
                                            desc.startVertex,
                                            desc.startInstance);
            }

            void DrawIndexed(const RhiDrawIndexedDesc& desc) override
            {
#if defined(_DEBUG)
                DebugIncrementDrawCount();
#endif
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

            void SetVertexBufferBindings(uint32_t startSlot, uint32_t count,
                                         const RhiVertexBufferBinding* bindings) override
            {
                if (count == 0 || !bindings) return;
                std::vector<D3D12_VERTEX_BUFFER_VIEW> dxViews(count);
                for (uint32_t i = 0; i < count; ++i) {
                    Resource* resource = m_device.GetD3D12CompatibilityResource(bindings[i].buffer);
                    if (!resource) {
                        continue;
                    }
                    dxViews[i].BufferLocation = resource->GetGPUVirtualAddress() + bindings[i].offsetInBytes;
                    dxViews[i].StrideInBytes  = bindings[i].strideInBytes;
                    dxViews[i].SizeInBytes    = bindings[i].sizeInBytes;
                }
                m_commandList.IASetVertexBuffers(startSlot, count, dxViews.data());
            }

            void SetIndexBufferBinding(const RhiIndexBufferBinding& binding) override
            {
                Resource* resource = m_device.GetD3D12CompatibilityResource(binding.buffer);
                if (!resource) {
                    return;
                }
                D3D12_INDEX_BUFFER_VIEW ibv{};
                ibv.BufferLocation = resource->GetGPUVirtualAddress() + binding.offsetInBytes;
                ibv.SizeInBytes    = binding.sizeInBytes;
                ibv.Format         = binding.is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
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

        const RayMarchRenderPass* FindRayMarchRenderPass(const std::vector<std::shared_ptr<IRenderPass>>& passes)
        {
            for (const auto& pass : passes) {
                if (!pass || pass->Tag() != "RayMarch") {
                    continue;
                }
                if (const auto* rayMarch = dynamic_cast<const RayMarchRenderPass*>(pass.get())) {
                    return rayMarch;
                }
            }
            return nullptr;
        }
    }

    bool Renderer::WaitForComputeFrameFence(UINT frameIndex)
    {
        if (!m_crossQueueFence || !m_computeFrameFenceEvent || frameIndex >= m_computeFrameFenceValues.size()) {
            return false;
        }

        const UINT64 fenceValue = m_computeFrameFenceValues[frameIndex];
        if (fenceValue == 0) {
            return true;
        }

        if (m_crossQueueFence->GetCompletedValue() < fenceValue) {
            if (FAILED(m_crossQueueFence->SetEventOnCompletion(fenceValue, m_computeFrameFenceEvent))) {
                return false;
            }
            const DWORD waitResult = WaitForSingleObject(m_computeFrameFenceEvent, INFINITE);
            if (waitResult != WAIT_OBJECT_0) {
                return false;
            }
        }
        return true;
    }

    void Renderer::Render(const OverlayRenderCallback& overlay)
    {
        Profiler::ScopedCpuEvent frameEvent("Renderer::Render");

        if (!m_device) {
            return;
        }

        if (UsesNativeBackendFrame(*m_device)) {
            if (m_deltaTime > 0.0f) {
                m_sceneTime += m_deltaTime;
            }
            RhiBackendFrameDesc frameDesc{};
            frameDesc.clearColor = { 0.2f, 0.2f, 0.2f, 1.0f };
            if (const RayMarchRenderPass* rayMarch = FindRayMarchRenderPass(m_passRegistry.GetPasses())) {
                auto& rayMarchDesc = frameDesc.rayMarch;
                rayMarchDesc.enabled = true;
                std::memcpy(rayMarchDesc.invViewProjection, m_cameraState.GetInvPV(), sizeof(rayMarchDesc.invViewProjection));
                const float* cameraPos = m_cameraState.GetPos();
                rayMarchDesc.cameraPos[0] = cameraPos[0];
                rayMarchDesc.cameraPos[1] = cameraPos[1];
                rayMarchDesc.cameraPos[2] = cameraPos[2];
                rayMarchDesc.sceneTimeSec = m_sceneTime;

                const auto light = m_lightSystem.GetDirectionalLightSettings();
                float lightFwd[3] = {};
                Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);
                rayMarchDesc.sunDir[0] = -lightFwd[0];
                rayMarchDesc.sunDir[1] = -lightFwd[1];
                rayMarchDesc.sunDir[2] = -lightFwd[2];
                rayMarchDesc.sunIntensity = light.intensity;
                rayMarchDesc.sunColor[0] = light.color[0];
                rayMarchDesc.sunColor[1] = light.color[1];
                rayMarchDesc.sunColor[2] = light.color[2];

                rayMarchDesc.cloudCover = rayMarch->GetCloudCover();
                rayMarchDesc.cloudDensity = rayMarch->GetCloudDensity();
                rayMarchDesc.debugMode = rayMarch->GetDebugMode();
                rayMarchDesc.renderWidth = m_viewport.Width;
                rayMarchDesc.renderHeight = m_viewport.Height;
                rayMarchDesc.tanHalfFovY = m_cameraState.GetTanHalfFovY();
                rayMarchDesc.aspectRatio = m_cameraState.GetAspectRatio();
                const float* right = m_cameraState.GetRight();
                const float* up = m_cameraState.GetUp();
                rayMarchDesc.cameraRight[0] = right[0];
                rayMarchDesc.cameraRight[1] = right[1];
                rayMarchDesc.cameraRight[2] = right[2];
                rayMarchDesc.cameraUp[0] = up[0];
                rayMarchDesc.cameraUp[1] = up[1];
                rayMarchDesc.cameraUp[2] = up[2];
                rayMarchDesc.explicitCameraBasis = (m_cameraState.GetCameraMode() == RenderCameraMode::RayMarch);
            } else if (!m_sceneSubmitter.GetDrawItems().empty() || !m_sceneSubmitter.GetSkinnedDrawItems().empty()) {
                std::vector<RhiBackendMeshDrawDesc> meshDraws;
                const auto& gpuItems = m_meshBuffer.Items();
                meshDraws.reserve(m_sceneSubmitter.GetDrawItems().size());
                for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                    if (item.meshIndex >= gpuItems.size()) {
                        continue;
                    }
                    const auto& gpuItem = gpuItems[item.meshIndex];
                    if (!gpuItem.rhiVb.IsValid()) {
                        continue;
                    }

                    RhiBackendMeshDrawDesc draw{};
                    draw.vertexBufferHandle = gpuItem.rhiVb.id;
                    draw.indexBufferHandle = gpuItem.rhiIb.id;
                    draw.vertexCount = gpuItem.vertexCount;
                    draw.indexCount = gpuItem.indexCount;
                    draw.vertexStride = gpuItem.vbv.StrideInBytes;
                    draw.indexSizeInBytes = gpuItem.ibv.SizeInBytes;
                    draw.index32Bit = (gpuItem.ibv.Format == DXGI_FORMAT_R32_UINT);
                    draw.transparent = item.transparent;
                    draw.albedoSrv = item.texture ? item.texture->srv.ptr : 0;
                    draw.occlusionSrv = item.occlusionTexture ? item.occlusionTexture->srv.ptr : 0;
                    std::memcpy(draw.model, item.model, sizeof(draw.model));
                    std::memcpy(draw.baseColor, item.material.baseColor, sizeof(draw.baseColor));
                    std::memcpy(draw.emissive, item.material.emissive, sizeof(draw.emissive));
                    draw.roughness = item.material.roughness;
                    draw.metallic = item.material.metallic;
                    meshDraws.push_back(draw);
                }

                std::vector<RhiBackendSkinnedMeshDrawDesc> skinnedDraws;
                const auto& skinnedGpuItems = m_skinnedMeshBuffer.Items();
                skinnedDraws.reserve(m_sceneSubmitter.GetSkinnedDrawItems().size());
                for (const auto& item : m_sceneSubmitter.GetSkinnedDrawItems()) {
                    if (item.meshIndex >= skinnedGpuItems.size()) {
                        continue;
                    }
                    const auto& gpuItem = skinnedGpuItems[item.meshIndex];
                    if (!gpuItem.rhiVb.IsValid() || item.boneMatricesNative.empty()) {
                        continue;
                    }

                    RhiBackendSkinnedMeshDrawDesc draw{};
                    draw.vertexBufferHandle = gpuItem.rhiVb.id;
                    draw.indexBufferHandle = gpuItem.rhiIb.id;
                    draw.vertexCount = gpuItem.vertexCount;
                    draw.indexCount = gpuItem.indexCount;
                    draw.vertexStride = gpuItem.vbv.StrideInBytes;
                    draw.indexSizeInBytes = gpuItem.ibv.SizeInBytes;
                    draw.index32Bit = (gpuItem.ibv.Format == DXGI_FORMAT_R32_UINT);
                    draw.transparent = item.transparent;
                    draw.albedoSrv = item.texture ? item.texture->srv.ptr : 0;
                    draw.occlusionSrv = item.occlusionTexture ? item.occlusionTexture->srv.ptr : 0;
                    std::memcpy(draw.model, item.model, sizeof(draw.model));
                    std::memcpy(draw.baseColor, item.material.baseColor, sizeof(draw.baseColor));
                    std::memcpy(draw.emissive, item.material.emissive, sizeof(draw.emissive));
                    draw.roughness = item.material.roughness;
                    draw.metallic = item.material.metallic;
                    draw.boneMatrices = item.boneMatricesNative.data();
                    skinnedDraws.push_back(draw);
                }

                if (!meshDraws.empty() || !skinnedDraws.empty()) {
                    auto& meshDesc = frameDesc.mesh;
                    meshDesc.enabled = true;
                    meshDesc.draws = meshDraws.data();
                    meshDesc.drawCount = static_cast<uint32_t>(meshDraws.size());
                    meshDesc.skinnedDraws = skinnedDraws.data();
                    meshDesc.skinnedDrawCount = static_cast<uint32_t>(skinnedDraws.size());
                    std::memcpy(meshDesc.viewProjection, m_cameraState.GetPV(), sizeof(meshDesc.viewProjection));
                    const float* cameraPos = m_cameraState.GetPos();
                    meshDesc.cameraPos[0] = cameraPos[0];
                    meshDesc.cameraPos[1] = cameraPos[1];
                    meshDesc.cameraPos[2] = cameraPos[2];
                    const auto light = m_lightSystem.GetDirectionalLightSettings();
                    float lightFwd[3] = {};
                    Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);
                    meshDesc.sunDir[0] = -lightFwd[0];
                    meshDesc.sunDir[1] = -lightFwd[1];
                    meshDesc.sunDir[2] = -lightFwd[2];
                    meshDesc.sunIntensity = light.intensity;
                    meshDesc.sunColor[0] = light.color[0];
                    meshDesc.sunColor[1] = light.color[1];
                    meshDesc.sunColor[2] = light.color[2];
                    meshDesc.renderWidth = m_viewport.Width;
                    meshDesc.renderHeight = m_viewport.Height;
                    meshDesc.enableIbl = true;
                    meshDesc.enableShadow = false;
                    meshDesc.enableSsao = (m_settings.runtimeAoEnabled || m_settings.ambientOcclusionMode != RendererEnums::AmbientOcclusionMode::MaterialOnly);
                    meshDesc.enablePostProcess = true;
                    frameDesc.present = true;
                    (void)overlay;
                    if (!m_device->ExecuteBackendFrame(frameDesc)) {
                        DebugLog("Renderer::Render: native backend mesh frame execution failed.\n");
                    }
                    return;
                }
            }
            frameDesc.present = true;
            (void)overlay;
            if (!m_device->ExecuteBackendFrame(frameDesc)) {
                DebugLog("Renderer::Render: native backend frame execution failed.\n");
            }
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

        // BeginFrame above waited on the fence for this back-buffer slot, i.e. the GPU has
        // finished frame (counter - GetBackBufferCount()). kFrameLatency is >= that, so the
        // frame we read here finished even earlier and its readback memory is settled.
        //
        // Order matters: kFrameLatency == the ring size, so (counter - kFrameLatency) maps to
        // the very slot BeginFrame is about to reuse. Reading it after would return this
        // frame's uninitialized metadata instead of the completed frame's results.
        // Increment first: the counter must already name the frame about to be recorded, because
        // the slot BeginFrame is about to claim is exactly the one holding frame
        // (thisFrame - kFrameLatency). Reading before the increment asks for one frame further
        // back than the ring actually still holds, and UpdateResults' appFrameIndex guard
        // (correctly) rejects every such request, leaving the results permanently empty.
        ++m_gpuProfilerFrameCounter;
        if (m_gpuProfilerFrameCounter >= GpuTimestampProfiler::kFrameLatency) {
            m_gpuTimestampProfiler.UpdateResults(
                m_gpuProfilerFrameCounter - GpuTimestampProfiler::kFrameLatency);
        }
        m_gpuTimestampProfiler.BeginFrame(m_gpuProfilerFrameCounter);
        const size_t drawItemCount = m_sceneSubmitter.GetDrawItems().size();
        const size_t skinnedDrawItemCount = m_sceneSubmitter.GetSkinnedDrawItems().size();
        // Raster shadow rendering records one draw per directional cascade, then the
        // main scene pass records another draw per item. Reserve the spot-shadow pass
        // as well so CSM4 cannot overwrite later per-draw camera constants. Skinned
        // items go through the same per-cascade + main + spot-shadow push sequence
        // (see drawSkinnedItems / drawSkinnedShadowItems below) so they need the same
        // per-item budget as static items.
        const size_t cameraCbPerItemSlots =
            static_cast<size_t>(LightSystem::kDirectionalCascadeCount) + 2u;
        // Point-light shadows record one draw per cube face (see LightSystem_Shadow.cpp's
        // "point light #0 cube shadow pass"), for both static and skinned items. VSM
        // directional modes record a second, VSM-only cascade pass, but only for static
        // items (skinned draws have no VSM color/moment pipeline).
        const bool hasPointLightShadow = !m_lightSystem.GetPointLights().empty();
        const size_t cameraCbPointShadowSlots = hasPointLightShadow
            ? (drawItemCount + skinnedDrawItemCount) * static_cast<size_t>(ShadowMapManager::kPointShadowFaceCount)
            : 0u;
        const DirectionalShadowMode shadowMode = m_lightSystem.GetDirectionalLightSettings().shadowMode;
        const bool isVsmShadowMode =
            shadowMode == DirectionalShadowMode::Vsm || shadowMode == DirectionalShadowMode::Vsm4;
        const size_t cameraCbVsmSlots = isVsmShadowMode
            ? drawItemCount * static_cast<size_t>(LightSystem::kDirectionalCascadeCount)
            : 0u;
        const size_t cameraCbDrawSlots =
            (drawItemCount + skinnedDrawItemCount) * cameraCbPerItemSlots +
            cameraCbPointShadowSlots + cameraCbVsmSlots;
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
        // This predicate decides whether to *allocate* the SSR render targets, whereas the
        // same-named predicate in Renderer.cpp decides whether to *execute* the SSR pass. It
        // intentionally omits IsValid() checks on the SSR targets themselves: this is the code
        // that creates them, so requiring them to already be valid would mean they could never
        // be created. When software ray traced reflections take over the frame, the SSR targets
        // are unused, so skip allocating them to avoid wasting VRAM.
        const bool softwareRayTracedReflectionsActive =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled &&
            m_renderTargetPool.GetSWRTReflectionTexture().IsValid();
        const bool needsScreenSpaceReflectionTargets =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterScreenSpaceReflectionEnabled &&
            !softwareRayTracedReflectionsActive;
        if (needsScreenSpaceReflectionTargets &&
            !m_renderTargetPool.EnsureScreenSpaceReflection(*m_device, sceneColorW, sceneColorH)) {
            DebugLog("Renderer::Render: failed to prepare screen-space reflection resources. Disabling SSR for this frame.\n");
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

        if (m_renderTargetPool.GetTransmissionSceneColorCopyTexture().IsValid()) {
            // Import TransmissionSceneColor so the graph's Read("TransmissionSceneColor")
            // (TransparentCompositeRenderPass) actually transitions it instead of being a
            // no-op dependency edge. TransparentSceneColorCopyRenderPass still writes it via
            // a manual CopyResource (CopySceneColorForTransmission) that already restores
            // PIXEL_SHADER_RESOURCE afterward, so this import never needs to issue its own
            // barrier -- it just lets the graph's bookkeeping agree with reality.
            ExternalRenderGraphResourceDesc transmissionSceneColorDesc{};
            transmissionSceneColorDesc.resource = m_renderTargetPool.GetTransmissionSceneColorCopyTexture().Get();
            transmissionSceneColorDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            transmissionSceneColorDesc.finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            transmissionSceneColorDesc.transitionToFinalState = true;
            transmissionSceneColorDesc.gpuSrv = m_renderTargetPool.GetTransmissionSceneColorCopySrv();
            transmissionSceneColorDesc.hasSrv = true;
            m_renderGraph.ImportExternalResource("TransmissionSceneColor", transmissionSceneColorDesc);
        }

        if (m_renderTargetPool.GetSSRSceneColorCopyTexture().IsValid() &&
            m_renderTargetPool.GetSSRReflectionTexture().IsValid()) {
            ExternalRenderGraphResourceDesc ssrSceneColorCopyDesc{};
            ssrSceneColorCopyDesc.resource = m_renderTargetPool.GetSSRSceneColorCopyTexture().Get();
            ssrSceneColorCopyDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ssrSceneColorCopyDesc.finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ssrSceneColorCopyDesc.transitionToFinalState = true;
            ssrSceneColorCopyDesc.gpuSrv = m_renderTargetPool.GetSSRSceneColorCopySrv();
            ssrSceneColorCopyDesc.hasSrv = true;
            m_renderGraph.ImportExternalResource("SSRSceneColorCopy", ssrSceneColorCopyDesc);

            ExternalRenderGraphResourceDesc ssrReflectionDesc{};
            ssrReflectionDesc.resource = m_renderTargetPool.GetSSRReflectionTexture().Get();
            ssrReflectionDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ssrReflectionDesc.finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ssrReflectionDesc.transitionToFinalState = true;
            ssrReflectionDesc.gpuSrv = m_renderTargetPool.GetSSRReflectionSrv();
            ssrReflectionDesc.hasSrv = true;
            m_renderGraph.ImportExternalResource("SSRReflection", ssrReflectionDesc);
        }

        ExternalRenderGraphResourceDesc backBufferDesc{};
        backBufferDesc.resource = backBuffer->Get();
        backBufferDesc.initialState = D3D12_RESOURCE_STATE_PRESENT;
        backBufferDesc.finalState = D3D12_RESOURCE_STATE_PRESENT;
        backBufferDesc.transitionToFinalState = true;
        backBufferDesc.rtv = m_renderTargetPool.GetBackBufferRtv(backIndex);
        backBufferDesc.hasRtv = true;
        backBufferDesc.clearColorOnFirstUse = false;
        m_renderGraph.ImportExternalResource("BackBuffer", backBufferDesc);

        static std::shared_ptr<FxaaRenderPass> s_fxaaPass = std::make_shared<FxaaRenderPass>();
        static bool s_fxaaPassRegistered = false;
        if (!s_fxaaPassRegistered) {
            m_passRegistry.AddPassAfter("PostProcess", s_fxaaPass);
            s_fxaaPassRegistered = true;
        }
        s_fxaaPass->SetEnabled(m_settings.fxaaEnabled);
        s_fxaaPass->SetBackBufferResource(backBuffer->Get());
        s_fxaaPass->EnsureResources(*m_device, sceneColorW, sceneColorH);

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

        // Import GBuffer resources so GBufferRenderPass can bind them as MRTs.
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
            importGBuffer("GBufferSpecularWorkflow", m_renderTargetPool.GetGBufferSpecularWorkflow(), m_renderTargetPool.GetGBufferSpecularWorkflowRtv(), m_renderTargetPool.GetGBufferSpecularWorkflowSrv());

            // Import SSAO / SSAOBlur so the graph owns their RTV<->SRV transitions instead of
            // hand-written barriers in SSAORenderPass.
            if (m_renderTargetPool.GetSSAOTexture().IsValid()) {
                importGBuffer("SSAO", m_renderTargetPool.GetSSAOTexture(), m_renderTargetPool.GetSSAORtv(), m_renderTargetPool.GetSSAOSrv());
            }
            if (m_renderTargetPool.GetSSAOBlurTexture().IsValid()) {
                importGBuffer("SSAOBlur", m_renderTargetPool.GetSSAOBlurTexture(), m_renderTargetPool.GetSSAOBlurRtv(), m_renderTargetPool.GetSSAOBlurSrv());
            }
        }

        D3D12CommandListRhiEncoder graphicsCommandEncoder(*m_device, *cmdList);

        // Runtime AO SRV selection is shared by the static and skinned draw lambdas: both bind
        // it at slot 8 (t9) once per batch, and the skinned transparent PS samples it too.
        auto selectRuntimeAoSrv = [this]() -> GpuDescriptorHandle {
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
            return runtimeAoSrv;
        };

        auto drawItems = [this, &graphicsCommandEncoder, frame, &selectRuntimeAoSrv](bool drawTransparent) {
            auto* enc = &graphicsCommandEncoder;
            // Bind runtime-generated AO texture at slot 8 (t9 in shader) once per batch.
            enc->SetGraphicsDescriptorTable(8, { selectRuntimeAoSrv().ptr });

            // Bind GI probe grid CB (b2) and probe SH data (t10) as inline root descriptors.
            // These are always bound so PBR_PS can read g_giEnabled to decide whether to use probes.
            if (m_readyState.IsFeatureReady(m_readyState.giReady) && m_probeGrid.IsInitialized()) {
                const RhiGpuAddress probeCbGpu = m_probeGrid.GetProbeGridCbGpuAddress();
                const RhiGpuAddress probeVA    = m_probeGrid.GetProbeDataGpuVA();
                if (probeCbGpu != 0) enc->SetGraphicsConstantBufferView(9, probeCbGpu);
                if (probeVA    != 0) enc->SetGraphicsShaderResourceView(10, probeVA);
            }

            // Frustum-cull against the camera before building the draw list. Bistro submits
            // ~1700 opaque meshes and every one of them used to be drawn every frame no matter
            // where the camera looked, which is most of the 8500 draw calls a frame was costing.
            //
            // Items whose bounds could not be computed (hasWorldBounds == false, e.g. a mesh with
            // no vertices) are always kept: a false positive only costs a draw, a false negative
            // makes geometry vanish.
            const Culling::FrustumPlanes cameraFrustum =
                Culling::ExtractFrustumPlanes(m_cameraState.GetPV());

            std::vector<const SceneSubmitter::DrawItem*> drawList;
            drawList.reserve(m_sceneSubmitter.GetDrawItems().size());
            size_t consideredCount = 0;
            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.transparent != drawTransparent) {
                    continue;
                }
                ++consideredCount;
                if (item.hasWorldBounds &&
                    !Culling::IsAabbVisible(cameraFrustum, item.worldBoundsMin, item.worldBoundsMax)) {
                    continue;
                }
                drawList.push_back(&item);
            }
            LogCullingStats(drawTransparent ? "Transparent" : "Opaque", consideredCount, drawList.size());
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
                    // y: double-sided flag. OpaqueGBuffer_PS negates the normal on back
                    // faces only when this is set, so single-sided materials keep their
                    // existing shading exactly.
                    item.material.doubleSided ? 1.0f : 0.0f,
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

                if (item.normalTexture) {
                    enc->SetGraphicsDescriptorTable(16, { item.normalTexture->srv.ptr });
                } else if (m_defaultNormalTexture) {
                    enc->SetGraphicsDescriptorTable(16, { m_defaultNormalTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(16, { m_nullTextureSrv.ptr });
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

        const Resource* lightCB = frame->light.GetLightCBResource();
        const D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu = (lightCB && lightCB->IsValid())
            ? lightCB->GetGPUVirtualAddress()
            : 0;
        const GpuDescriptorHandle defaultAoSrv =
            (m_defaultOcclusionTexture != nullptr) ? m_defaultOcclusionTexture->srv : m_nullTextureSrv;
        const SWRTExecutor::PartialBehavior partialBehavior =
            m_swrtExecutor.ResolveBehavior(m_settings.rayTracingPerformancePreset);
        const bool swrtReadyForFrame = m_readyState.IsFeatureReady(m_readyState.swrtReady);
        const bool useSoftwareRayTracedDirectionalShadow =
            swrtReadyForFrame &&
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedDirectionalShadowEnabled;
        const bool useSoftwareRayTracedReflections =
            swrtReadyForFrame &&
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled;
        const bool useSoftwareRayTracedAmbientOcclusion =
            swrtReadyForFrame &&
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

        const bool hasLightingPass =
            std::any_of(m_passRegistry.GetPasses().begin(),
                        m_passRegistry.GetPasses().end(),
                        [](const std::shared_ptr<IRenderPass>& runtimePass) {
                            if (!runtimePass) {
                                return false;
                            }
                            const std::string_view tag = runtimePass->Tag();
                            return tag == "Lighting" || tag == "TransparentLighting";
                        });
        const bool useShadowTessPath = hasLightingPass && m_settings.useTessellation;

        // Accumulated across the four cascade invocations of drawShadowItems within this frame.
        // Two questions this answers, which the GPU timestamp alone cannot:
        //  - How long the CPU spends RECORDING the shadow draws. The frame is recorded into one
        //    command list and submitted once at frame end, so recording finishes before the GPU
        //    starts and cannot stall it mid-pass; if CPU record time is far below the GPU span,
        //    the GPU span is genuine GPU work rather than starvation.
        //  - How many triangles those draws carry. Reducing cascade count lowers draw count and
        //    triangle count together, so the measured linearity in draw count is confounded and
        //    cannot by itself attribute the cost to per-draw overhead.
        struct ShadowRecordStats
        {
            double   recordMicroseconds = 0.0;
            uint64_t triangles          = 0;
            uint64_t draws              = 0;
        };
        ShadowRecordStats shadowRecordStats{};

        auto drawShadowItems = [this, &graphicsCommandEncoder, frame, &shadowRecordStats](const LightSystem::ShadowPassContext& context) {
            const auto recordBegin = std::chrono::steady_clock::now();
            auto* enc = &graphicsCommandEncoder;
            // Per-cascade culling. This is the whole point of having cascades: cascade 0 covers a
            // few metres, so drawing the entire city into it is pure waste. Before this, the
            // shadow pass issued exactly meshCount * cascadeCount draws (1698 * 4 = 6792 measured),
            // i.e. no culling whatsoever.
            //
            // The near plane is deliberately ignored. An object that sits between the light and
            // the cascade box is outside the box but still casts a shadow INTO it; clipping it
            // against the near plane would make that shadow pop out of existence.
            const Culling::FrustumPlanes cascadeFrustum =
                Culling::ExtractFrustumPlanes(context.lightViewProjection);

            size_t shadowConsidered = 0;
            size_t shadowDrawn     = 0;

            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.transparent) {
                    continue;
                }
                ++shadowConsidered;
                if (item.hasWorldBounds &&
                    !Culling::IsAabbVisibleIgnoringNearPlane(cascadeFrustum,
                                                             item.worldBoundsMin,
                                                             item.worldBoundsMax)) {
                    continue;
                }
                ++shadowDrawn;
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
                        shadowRecordStats.triangles += it.indexCount / 3u;
                        ++shadowRecordStats.draws;
                    } else if (it.vertexCount > 0) {
                        enc->Draw({ it.vertexCount, 1, 0, 0 });
                        shadowRecordStats.triangles += it.vertexCount / 3u;
                        ++shadowRecordStats.draws;
                    }
                }
            }

            shadowRecordStats.recordMicroseconds +=
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - recordBegin).count();

            // Reported on the last cascade so the numbers cover a whole frame's shadow recording.
            if (context.activeCascadeIndex + 1u >= context.cascadeCount) {
                static unsigned int s_shadowStatSample = 0u;
                if ((s_shadowStatSample++ % 240u) == 0u) {
                    char statLine[224];
                    std::snprintf(statLine, sizeof(statLine),
                                  "[ShadowRecord] cpuRecord=%.2fms draws=%llu tris=%llu (cpuPerDraw=%.2fus)\n",
                                  shadowRecordStats.recordMicroseconds / 1000.0,
                                  static_cast<unsigned long long>(shadowRecordStats.draws),
                                  static_cast<unsigned long long>(shadowRecordStats.triangles),
                                  shadowRecordStats.draws > 0
                                      ? (shadowRecordStats.recordMicroseconds / static_cast<double>(shadowRecordStats.draws))
                                      : 0.0);
                    DebugLog(statLine);
                }
                shadowRecordStats = ShadowRecordStats{};
            }

            // Labelled per cascade: the four cascades have very different frustum volumes, so a
            // single merged "ShadowCascade" number cannot show which cascade dominates the cost.
            char cascadeLabel[32];
            std::snprintf(cascadeLabel, sizeof(cascadeLabel), "ShadowCascade[%u]", context.activeCascadeIndex);
            LogCullingStats(cascadeLabel, shadowConsidered, shadowDrawn);
        };

        const RenderPassExecutionPolicy executionPolicy = BuildRenderPassExecutionPolicy(useShadowTessPath);
        // --- Build frame inputs for graphics and (optionally) compute queues ---
        RenderPassFrameInputs frameInputs = BuildRenderPassFrameInputs(
            cmdList,
            &graphicsCommandEncoder,
            frame,
            lightCbGpu,
            defaultAoSrv);

        // Prepare compute command list if async compute is available.
        CommandList* computeCmdList = nullptr;
        std::unique_ptr<D3D12CommandListRhiEncoder> computeCommandEncoder;
        RenderPassFrameInputs computeFrameInputs{};
        const UINT computeSlot = backIndex % (m_computeAllocators.empty() ? 1u : static_cast<UINT>(m_computeAllocators.size()));
        if (m_computeCmdListReady && !m_computeAllocators.empty() && WaitForComputeFrameFence(computeSlot)) {
            CommandAllocator& computeAlloc = m_computeAllocators[computeSlot];
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

        auto drawSkinnedItems = [this, &graphicsCommandEncoder, frame, &selectRuntimeAoSrv](bool drawTransparent) {
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

            // Runtime AO (t9) and the GI probe grid (b2 / t10) are per-draw-list state, not
            // per-item. The non-skinned lambda binds them once before its loop; the skinned
            // path used to skip them entirely, so skinned transparent draws -- whose PS is
            // CookTorranceGGX_PS via TransparentOIT_PS, and which samples RuntimeAOTex and
            // GI_SampleProbeGrid -- read undefined descriptors. The skinned root signature
            // copies parameters 0-15 from the shared one, so the indices match.
            {
                auto* enc = &graphicsCommandEncoder;
                enc->SetGraphicsDescriptorTable(8, { selectRuntimeAoSrv().ptr });
                if (m_readyState.IsFeatureReady(m_readyState.giReady) && m_probeGrid.IsInitialized()) {
                    const RhiGpuAddress probeCbGpu = m_probeGrid.GetProbeGridCbGpuAddress();
                    const RhiGpuAddress probeVA    = m_probeGrid.GetProbeDataGpuVA();
                    if (probeCbGpu != 0) enc->SetGraphicsConstantBufferView(9, probeCbGpu);
                    if (probeVA    != 0) enc->SetGraphicsShaderResourceView(10, probeVA);
                }
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
                    // y: double-sided flag. OpaqueGBuffer_PS negates the normal on back
                    // faces only when this is set, so single-sided materials keep their
                    // existing shading exactly.
                    item.material.doubleSided ? 1.0f : 0.0f,
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
                    enc->SetGraphicsConstantBufferView(16, item.boneMatricesCbGpu);
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

                if (item.normalTexture) {
                    enc->SetGraphicsDescriptorTable(17, { item.normalTexture->srv.ptr });
                } else if (m_defaultNormalTexture) {
                    enc->SetGraphicsDescriptorTable(17, { m_defaultNormalTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(17, { m_nullTextureSrv.ptr });
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

        auto drawSkinnedShadowItems = [this, &graphicsCommandEncoder, frame](const LightSystem::ShadowPassContext& context) {
            auto* enc = &graphicsCommandEncoder;
            for (const auto& item : m_sceneSubmitter.GetSkinnedDrawItems()) {
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
                if (item.boneMatricesCbGpu != 0) {
                    enc->SetGraphicsConstantBufferView(16, item.boneMatricesCbGpu);
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

        RenderPassExecutionServices executionServices = BuildRenderPassExecutionServices(
            drawItems,
            drawShadowItems);
        if (!m_sceneSubmitter.GetSkinnedDrawItems().empty()) {
            executionServices.drawSkinnedOpaqueItems      = [drawSkinnedItems]() { drawSkinnedItems(false); };
            executionServices.drawSkinnedTransparentItems = [drawSkinnedItems]() { drawSkinnedItems(true); };
            executionServices.drawSkinnedShadowItems      = drawSkinnedShadowItems;
        }
        executionServices.toneMapSceneColor = [this, cmdList, backIndex]() {
            return ToneMapSceneColor(cmdList, backIndex);
        };
        executionServices.copySceneColorForTransmission = [this, cmdList]() {
            return CopySceneColorForTransmission(cmdList);
        };
        executionServices.executeSoftwareDirectionalShadow = [this, cmdList, partialBehavior](const LightSystem::ShadowPassContext& shadowContext) {
            if (!m_readyState.IsFeatureReady(m_readyState.swrtReady)) {
                return false;
            }
            const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteDirectionalShadow(cmdList, shadowContext, ctx, partialBehavior, m_settings, m_rayTracingStats);
        };
        executionServices.executeSoftwareReflections = [this, cmdList, partialBehavior]() {
            if (!m_readyState.IsFeatureReady(m_readyState.swrtReady)) {
                return false;
            }
            const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteReflections(cmdList, ctx, partialBehavior, m_settings, m_rayTracingStats);
        };
        executionServices.executeRayTracing = [this, cmdList, backIndex]() {
            if (!m_readyState.IsFeatureReady(m_readyState.dxrReady)) {
                return false;
            }
            // Lazy DXR scene build: SceneSubmitter no longer rebuilds acceleration
            // structures at submit time (that stalled scene streaming with WaitForGPU
            // even when hardware RT was unused). UpdateScene's internal version check
            // makes this a no-op when the scene hasn't changed.
            m_dxrRayTracer.UpdateScene(*m_device, m_rayTracingScene);
            const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
            return m_swrtExecutor.ExecuteHardware(cmdList, backIndex, ctx, m_settings, m_rayTracingStats);
        };

        m_passRegistry.ClearPhaseCompletionNodes();
        if (useSoftwareRayTracedAmbientOcclusion) {
            m_passRegistry.AddPhaseCompletionNode(
                "Scene",
                "SwrtAmbientOcclusion",
                [this, cmdList](const RenderPassContextView&) -> bool {
                    const auto ctx = m_sceneSynchronizer.BuildSwrtFrameContext();
                    return m_swrtExecutor.ExecuteAmbientOcclusion(cmdList, ctx, m_settings, m_rayTracingStats);
                },
                PhaseCompletionMode::Deterministic,
                {});
        }
        if (useSoftwareRayTracedReflections) {
            if (m_settings.gBufferDebugView == RendererEnums::GBufferDebugView::FinalLit ||
                m_settings.gBufferDebugView == RendererEnums::GBufferDebugView::SwrtReflectionComposite) {
                executionServices.compositeSoftwareReflections =
                    [this, cmdList, backIndex, lightCbGpu]() -> bool {
                        return CompositeSoftwareReflections(cmdList, backIndex, lightCbGpu);
                    };
            }
        }

        // Reallocate the fluid/particle buffers (if their capacity changed) before the
        // render graph is registered/executed below. RegisterPassesToRenderGraph's pass
        // lambdas capture `this` and resolve GetCurrentHeightGpuVA()/GetParticleBufferGpuVA()
        // only when Execute() actually runs them, so any reallocation must complete before
        // that point — otherwise Execute() could record a read of a buffer that Update()
        // (called later this same frame, see below) then destroys before the command list
        // is closed/submitted, corrupting the command list's resource references.
        if (m_readyState.IsFeatureReady(m_readyState.fluidReady) && m_fluidSim.IsInitialized()) m_fluidSim.PrepareFrame(*m_device);
        if (m_readyState.IsFeatureReady(m_readyState.particlesReady) && m_particleSystem.IsInitialized()) m_particleSystem.PrepareFrame(*m_device);

        // Per-Node graphics command list rotation. Only engaged by RenderGraph::Execute()
        // at a genuine cross-queue join (a level with both a compute-preferred pass and an
        // available compute queue) — see the callbacks below. With zero passes preferring
        // the compute queue today, this path is never invoked and `cmdList` keeps pointing
        // at the single frame-long command list BeginFrame opened, so behaviour is unchanged.
        // extraGraphicsEncoders keeps each rotated node's encoder alive for the rest of the
        // frame, since frameInputs.execution.commandEncoder only stores a raw pointer to it.
        std::vector<std::unique_ptr<D3D12CommandListRhiEncoder>> extraGraphicsEncoders;
        size_t nextGraphicsNodeSlot = 0;
        auto submitCurrentGraphicsNode = [&]() -> bool {
            if (!cmdList) {
                return false;
            }
            if (FAILED(cmdList->Close())) {
                return false;
            }
            ID3D12CommandList* lists[] = { cmdList->Get() };
            m_device->GetCommandQueue().ExecuteCommandLists(1, lists);
            return true;
        };
        auto acquireNextGraphicsNode = [&]() -> bool {
            if (backIndex >= m_graphicsNodeCommandLists.size()) {
                return false;
            }
            auto& slots = m_graphicsNodeCommandLists[backIndex];
            if (nextGraphicsNodeSlot >= slots.size()) {
                slots.emplace_back();
            }
            GraphicsNodeCommandList& slot = slots[nextGraphicsNodeSlot];
            ++nextGraphicsNodeSlot;

            if (!slot.allocator.Get()) {
                if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator))) {
                    return false;
                }
            } else if (FAILED(slot.allocator.Reset())) {
                return false;
            }

            if (!slot.cmdList.Get()) {
                if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator,
                                                       &m_pipelineStateCache.GetPipelineState(), slot.cmdList))) {
                    return false;
                }
            } else if (FAILED(slot.cmdList.Reset(slot.allocator, &m_pipelineStateCache.GetPipelineState()))) {
                return false;
            }

            extraGraphicsEncoders.push_back(std::make_unique<D3D12CommandListRhiEncoder>(*m_device, slot.cmdList));
            cmdList = &slot.cmdList;
            frameInputs.execution.cmdList        = cmdList;
            frameInputs.execution.commandEncoder = extraGraphicsEncoders.back().get();
            return true;
        };

        RenderGraphExecuteContext executeContext{};
        executeContext.executionPolicy    = &executionPolicy;
        executeContext.frameInputs        = &frameInputs;
        executeContext.computeFrameInputs = computeCmdList ? &computeFrameInputs : nullptr;
        executeContext.executionServices  = &executionServices;
        executeContext.resources          = &m_renderGraph.GetResourceRegistry();
        executeContext.gpuTimestampProfiler = &m_gpuTimestampProfiler;
        if (computeCmdList && m_crossQueueFence) {
            executeContext.graphicsQueueRaw   = m_device->GetCommandQueue().Get();
            executeContext.computeQueueRaw    = m_device->GetComputeQueue().Get();
            executeContext.crossQueueFence    = m_crossQueueFence.Get();
            executeContext.crossQueueFenceVal = &m_crossQueueFenceVal;
            executeContext.submitCurrentGraphicsNode = submitCurrentGraphicsNode;
            executeContext.acquireNextGraphicsNode   = acquireNextGraphicsNode;
        }

        if (!m_passRegistry.RegisterPassesToRenderGraph(m_renderGraph, executeContext, m_settings.renderPathMode, m_rayTracingRenderPass)) {
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

        // Submit compute CL (SWRT work) on the compute queue. Any mid-frame
        // cross-queue join for this frame (RenderGraph::Execute's Signal/Wait pair)
        // has already ordered this submission relative to the graphics queue's Node
        // submissions above; it does not by itself order this *frame-closing*
        // submission against anything after it. Signal here and record the value
        // so a later frame's WaitForComputeFrameFence() can safely Reset() this
        // slot's allocator/command list once the GPU has actually finished this.  
        if (computeCmdList) {
            if (SUCCEEDED(computeCmdList->Close())) {
                ID3D12CommandList* cLists[] = { computeCmdList->Get() };
                m_device->GetComputeQueue().ExecuteCommandLists(1, cLists);
                if (m_crossQueueFence && computeSlot < m_computeFrameFenceValues.size()) {
                    const UINT64 fenceValue = ++m_crossQueueFenceVal;
                    if (SUCCEEDED(m_device->GetComputeQueue().Signal(m_crossQueueFence.Get(), fenceValue))) {
                        m_computeFrameFenceValues[computeSlot] = fenceValue;
                    }
                }
            }
        }

        if (!graphExecuted) {
            if (overlay) {
                TransitionBackBufferToRenderTarget(cmdList, backIndex);
                ClearAndBindMainTargets(cmdList, backIndex);
                overlay(*cmdList, m_renderTargetPool.GetBackBufferRtv(backIndex));
                TransitionBackBufferToPresent(cmdList, backIndex);
            }
            SubmitAndPresent(cmdList, backIndex);
            return;
        }

        CaptureSceneColorHistory(cmdList, backIndex);

        // Advance the fluid heightfield simulation after the render graph has read this
        // frame's state (FluidSurface_VS samples GetCurrentHeightGpuVA/GetUpdateCBGpuVA);
        // the updated buffer becomes visible starting next frame (1-frame lag), mirroring
        // the GI probe update timing below.
        if (m_readyState.IsFeatureReady(m_readyState.fluidReady) && m_fluidSim.IsInitialized() && cmdList) {
            m_fluidSim.Update(m_deltaTime, *m_device, *cmdList);
        }
        if (m_readyState.IsFeatureReady(m_readyState.particlesReady) && m_particleSystem.IsInitialized() && cmdList) m_particleSystem.Update(m_deltaTime, *m_device, *cmdList);

        // GI probe bake in Raster mode (or explicit bake request in any mode).
        if (m_giBakeRequested) {
            if (!m_readyState.IsFeatureReady(m_readyState.giReady) || !m_probeGrid.IsInitialized()) {
                SetGIBakePhase("Render tail: waiting for probe grid");
                if (m_giBakeStatus.stalledFrames == 0u ||
                    ((m_giBakeStatus.stalledFrames + 1u) % 60u) == 0u) {
                    AddGIBakeLog("ProbeGrid", "Waiting for GI probe grid initialization. stalledFrames=%u",
                                 m_giBakeStatus.stalledFrames + 1u);
                }
                RefreshGIBakeStatus(GIBakeState::WaitingForProbeGrid);
            } else if (m_probeGrid.IsBaked()) {
                if (m_giContinuousMode) {
                    const uint32_t total = m_probeGrid.GetTotalProbeCount();
                    m_probeGrid.ResetBakeState();
                    m_probeGrid.FlushGridCB();
                    SetGIBakePhase("Render tail: continuous refresh");
                    RefreshGIBakeStatus(GIBakeState::Continuous);
                    AddGIBakeLog("Continuous", "Full pass complete (%u probes); restarting for continuous refresh.", total);
                } else {
                    m_giBakeRequested = false;
                    SetGIBakePhase("Render tail: completed");
                    m_probeGrid.FlushGridCB();
                    RefreshGIBakeStatus(GIBakeState::Completed);
                    AddGIBakeLog("Completed", "Bake already completed before dispatch. probes=%u/%u",
                                 m_probeGrid.GetBakedProbeCount(),
                                 m_probeGrid.GetTotalProbeCount());
                }
            } else {
                if (m_giBakeClearPending) {
                    m_giBakeClearPending = false;
                }

                if (m_giBakeRequested) {
                    SetGIBakePhase("Render tail: updating SWRT BVH");
                    if (m_giBakeFrameIndex == 0u && m_probeGrid.GetBakedProbeCount() == 0u) {
                        AddGIBakeLog("BVH Update", "Begin SWRT BVH update. scene=%u instances / %u triangles, versions=%llu/%llu/%llu",
                                     static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                                     m_rayTracingScene.TriangleCount(),
                                     static_cast<unsigned long long>(m_rayTracingScene.geometryVersion),
                                     static_cast<unsigned long long>(m_rayTracingScene.materialVersion),
                                     static_cast<unsigned long long>(m_rayTracingScene.instanceVersion));
                    }
                    const bool swrtReadyForBvhUpdate = m_readyState.IsFeatureReady(m_readyState.swrtReady);
                    if (swrtReadyForBvhUpdate) {
                        m_gpuSoftwareRayTracer.UpdateScene(m_rayTracingScene, *m_device, *cmdList);
                    }
                    const auto bvhAddrs = swrtReadyForBvhUpdate
                        ? m_gpuSoftwareRayTracer.GetBvhGpuAddresses()
                        : GpuSoftwareRayTracer::BvhGpuAddresses{};
                    if (bvhAddrs.valid) {
                        SetGIBakePhase("Render tail: dispatching GI probes");
                        const auto& bvhDiag = m_gpuSoftwareRayTracer.GetBvhBuildDiagnostics();
                        if (m_giBakeLastLoggedMissingMask != 0u) {
                            AddGIBakeLog("BVH Ready", "SWRT BVH buffers ready. phase=%s, scene=%u instances / %u triangles, meshBVH=%u, tlasNodes=%u",
                                         bvhDiag.lastPhase,
                                         static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                                         m_rayTracingScene.TriangleCount(),
                                         bvhDiag.meshBvhCount,
                                         bvhDiag.tlasNodeCount);
                        }
                        m_giBakeLastLoggedMissingMask = 0u;
                        const auto dirLight = m_lightSystem.GetDirectionalLightSettings();
                        float fwd[3];
                        Math::DirectionFromYawPitch(dirLight.yaw, dirLight.pitch, fwd);
                        IrradianceProbeGrid::UpdateDesc giDesc{};
                        giDesc.dirLightDir[0]     = -fwd[0];
                        giDesc.dirLightDir[1]     = -fwd[1];
                        giDesc.dirLightDir[2]     = -fwd[2];
                        giDesc.dirLightIntensity  = dirLight.intensity;
                        // Probe rays evaluate point/spot lights too, so indirect light
                        // picks up local sources instead of only the sun. Null here means
                        // zero lights, which is exactly the previous behaviour.
                        giDesc.pointLights        = &m_lightSystem.GetPointLights();
                        giDesc.spotLights         = &m_lightSystem.GetSpotLights();
                        giDesc.dirLightColor[0]   = dirLight.color[0];
                        giDesc.dirLightColor[1]   = dirLight.color[1];
                        giDesc.dirLightColor[2]   = dirLight.color[2];
                        giDesc.ambientColor[0]    = 0.1f;
                        giDesc.ambientColor[1]    = 0.1f;
                        giDesc.ambientColor[2]    = 0.1f;
                        giDesc.ambientIntensity   = 1.0f;
                        giDesc.shadowBias         = 0.005f;
                        giDesc.frameIndex         = m_giBakeFrameIndex++;

                        const uint32_t beforeProbes = m_probeGrid.GetBakedProbeCount();
                        if (!m_probeGrid.UpdateProbes(giDesc, bvhAddrs, *m_device, *cmdList)) {
                            m_giBakeRequested = false;
                            SetGIBakePhase("Render tail: probe dispatch failed");
                            RefreshGIBakeStatus(GIBakeState::Failed);
                            AddGIBakeLog("ProbeDispatch", "UpdateProbes failed after BVH was ready. probes=%u/%u",
                                         beforeProbes,
                                         m_probeGrid.GetTotalProbeCount());
                        } else if (m_probeGrid.IsBaked()) {
                            if (m_giContinuousMode) {
                                const uint32_t total = m_probeGrid.GetTotalProbeCount();
                                m_probeGrid.ResetBakeState();
                                SetGIBakePhase("Render tail: continuous refresh");
                                RefreshGIBakeStatus(GIBakeState::Continuous);
                                AddGIBakeLog("Continuous", "Full pass complete (%u probes); restarting for continuous refresh.", total);
                            } else {
                                m_giBakeRequested = false;
                                SetGIBakePhase("Render tail: completed");
                                RefreshGIBakeStatus(GIBakeState::Completed);
                                AddGIBakeLog("Completed", "Final probe dispatch succeeded. probes=%u/%u",
                                             m_probeGrid.GetBakedProbeCount(),
                                             m_probeGrid.GetTotalProbeCount());
                            }
                        } else {
                            const uint32_t afterProbes = m_probeGrid.GetBakedProbeCount();
                            if (afterProbes != m_giBakeLastLoggedCompletedProbes) {
                                AddGIBakeLog("ProbeDispatch", "Dispatched probes %u-%u. progress=%u/%u",
                                             beforeProbes,
                                             afterProbes > 0u ? afterProbes - 1u : 0u,
                                             afterProbes,
                                             m_probeGrid.GetTotalProbeCount());
                                m_giBakeLastLoggedCompletedProbes = afterProbes;

                                if (afterProbes == 0u && !m_probeGrid.GetLastPipelineError().empty()) {
                                    AddGIBakeLog("PipelineError", "%s",
                                                 m_probeGrid.GetLastPipelineError().c_str());
                                }
                            }
                            RefreshGIBakeStatus(GIBakeState::Baking);
                        }
                    } else {
                        SetGIBakePhase("Render tail: waiting for SWRT BVH");
                        const auto& bvhDiag = m_gpuSoftwareRayTracer.GetBvhBuildDiagnostics();
                        const std::string missing = FormatGIBvhMissingMask(bvhAddrs.missingMask);
                        if (bvhAddrs.missingMask & GI_BVH_MISSING_SWRT_NOT_INITIALIZED) {
                            m_giBakeRequested = false;
                            SetGIBakePhase("Render tail: SWRT unavailable");
                            RefreshGIBakeStatus(GIBakeState::Failed);
                            AddGIBakeLog("BVH Failed", "SWRT is not initialized; BakeGI cannot build BVH.");
                        } else {
                            const bool shouldLogWait =
                                bvhAddrs.missingMask != m_giBakeLastLoggedMissingMask ||
                                m_giBakeStatus.stalledFrames == 0u ||
                                ((m_giBakeStatus.stalledFrames + 1u) % 60u) == 0u;
                            if (shouldLogWait) {
                                AddGIBakeLog("WaitingForBvh", "Retrying SWRT BVH upload; missing=%s, phase=%s, lastFailure=%s, stalledFrames=%u, scene=%u instances / %u triangles",
                                             missing.c_str(),
                                             bvhDiag.lastPhase,
                                             bvhDiag.lastFailure[0] ? bvhDiag.lastFailure : "none",
                                             m_giBakeStatus.stalledFrames + 1u,
                                             static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                                             m_rayTracingScene.TriangleCount());
                                m_giBakeLastLoggedMissingMask = bvhAddrs.missingMask;
                            }
                            RefreshGIBakeStatus(GIBakeState::WaitingForBvh, bvhAddrs.missingMask);
                        }
                    }
                }
            }
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


} // namespace SasamiRenderer
