// RendererPassServices.cpp
// Renderer::BuildFramePassInputsAndServices -- stage 5 of Renderer::Render's frame graph
// construction: builds the RenderPassExecutionPolicy/FrameInputs/ExecutionServices,
// the optional async-compute frame inputs, the skinned-mesh draw/shadow callbacks, the
// virtual-geometry cluster draw services, the software-ray-tracing/tone-map/composite
// services, and the cross-queue graphics-node rotation callbacks, then assembles the
// RenderGraphExecuteContext that stage 6 (still in RendererFrameGraph.cpp) registers
// passes into. See Renderer.h for why this lives in its own file, and
// FrameRenderContext.h for why this and stage 4 (RendererDrawCallbacks.cpp) -- and the
// stages that stay in RendererFrameGraph.cpp -- hand state to each other through a
// shared context struct instead of ordinary locals.
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Runtime/FrameRenderContext.h"
#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"
#include "Renderer/VirtualGeometry/ClusterDrawSubmitter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include "Foundation/Math/MathUtil.h"

// windows.h (pulled in transitively by the D3D12 headers) defines min/max as macros, which
// breaks std::max and std::numeric_limits<T>::max(). Same guard as RendererFrameGraph.cpp.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif


namespace SasamiEngine::Render
{
    using Math::Mul4x4;

    namespace
    {
        // Face-shadow frame for the mesh-shader GBuffer PSs; mirrors what LightSystemShadow.cpp
        // writes into LightCB (dirDir = light forward, so the PS wants its negation).
        ClusterToonFaceShadow BuildClusterToonFaceShadow(const LightSystem& lightSystem, const Texture* faceMap,
                                                         const SrvDescriptorAllocator& srvAllocator)
        {
            ClusterToonFaceShadow face{};
            const LightSystem::ToonFaceSettings& settings = lightSystem.GetToonFaceSettings();
            face.enabled = settings.enabled;
            const LightSystem::DirectionalLightSettings light = lightSystem.GetDirectionalLightSettings();
            float lightForward[3] = {};
            Math::DirectionFromYawPitch(light.yaw, light.pitch, lightForward);
            for (int axis = 0; axis < 3; ++axis) {
                face.toLight[axis] = -lightForward[axis];
                face.forward[axis] = settings.forward[axis];
                face.right[axis] = settings.right[axis];
            }
            face.softness = settings.softness;
            if (faceMap) {
                face.mapSrv = faceMap->srv;
                face.mapSrvIndex = srvAllocator.GetIndex(faceMap->srv);
            }
            return face;
        }
    }

    void Renderer::BuildFramePassInputsAndServices(FrameRenderContext& context)
    {
        // Local aliases bound directly to FrameRenderContext's own storage (or, for
        // cmdList, to Renderer::Render's own local through the context's double
        // pointer). Reference-capturing any of these names in a lambda below binds to
        // that underlying storage, not to this function's stack frame, so it stays
        // valid for lambdas the render-pass graph invokes after this function returns
        // (see FrameRenderContext.h).
        CommandList*& cmdList = *context.cmdList;
        auto* const frame = context.frame;
        const UINT& backIndex = context.backIndex;
        D3D12CommandListRhiEncoder& graphicsCommandEncoder = *context.graphicsCommandEncoder;
        auto& selectRuntimeAoSrv = context.selectRuntimeAoSrv;
        const D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu = context.lightCbGpu;
        const GpuDescriptorHandle defaultAoSrv = context.defaultAoSrv;
        const SWRTExecutor::PartialBehavior& partialBehavior = context.partialBehavior;
        const bool useSoftwareRayTracedReflections = context.useSoftwareRayTracedReflections;
        const bool useSoftwareRayTracedAmbientOcclusion = context.useSoftwareRayTracedAmbientOcclusion;

        context.executionPolicy = BuildRenderPassExecutionPolicy(context.useShadowTessPath);
        const RenderPassExecutionPolicy& executionPolicy = context.executionPolicy;
        // --- Build frame inputs for graphics and (optionally) compute queues ---
        context.frameInputs = BuildRenderPassFrameInputs(
            cmdList,
            &graphicsCommandEncoder,
            frame,
            lightCbGpu,
            defaultAoSrv);
        RenderPassFrameInputs& frameInputs = context.frameInputs;

        // Prepare compute command list if async compute is available.
        CommandList*& computeCmdList = context.computeCmdList;
        std::unique_ptr<D3D12CommandListRhiEncoder>& computeCommandEncoder = context.computeCommandEncoder;
        RenderPassFrameInputs& computeFrameInputs = context.computeFrameInputs;
        context.computeSlot = backIndex % (m_computeAllocators.empty() ? 1u : static_cast<UINT>(m_computeAllocators.size()));
        const UINT& computeSlot = context.computeSlot;
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
                    GetEffectiveAmbientOcclusionMode();
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
                    EncodeShadingSelector(item.material),
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
                    // z: glTF MASK alpha cutoff (0 = no clip test).
                    GetGBufferAlphaCutoff(item.material),
                    // w: SDF face-shadow flag.
                    item.material.toonFaceShadow ? 1.0f : 0.0f,
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

                if (item.emissiveTexture) {
                    enc->SetGraphicsDescriptorTable(18, { item.emissiveTexture->srv.ptr });
                } else if (m_defaultEmissiveTexture) {
                    enc->SetGraphicsDescriptorTable(18, { m_defaultEmissiveTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(18, { m_nullTextureSrv.ptr });
                }

                // t19 SDF face-shadow map: scene-wide, white when unset (the shader gates on flags).
                if (m_toonFaceShadowMap) {
                    enc->SetGraphicsDescriptorTable(19, { m_toonFaceShadowMap->srv.ptr });
                } else if (m_defaultEmissiveTexture) {
                    enc->SetGraphicsDescriptorTable(19, { m_defaultEmissiveTexture->srv.ptr });
                } else {
                    enc->SetGraphicsDescriptorTable(19, { m_nullTextureSrv.ptr });
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

        auto drawSkinnedShadowItems = [this, cmdList, &graphicsCommandEncoder, frame](const LightSystem::ShadowPassContext& shadowContext) {
            auto* enc = &graphicsCommandEncoder;
            using SkinnedItem = std::decay_t<decltype(m_sceneSubmitter.GetSkinnedDrawItems().front())>;
            // glTF MASK casters, drawn after the rest with the alpha-tested PSO (same as the static callback).
            std::vector<const SkinnedItem*> alphaMaskItems;

            const auto drawSkinnedShadowMesh = [&](const SkinnedItem& item, const float* maskExtra0,
                                                   const float* maskExtra6) {
                float objLightMVP[16];
                Mul4x4(item.model, shadowContext.lightViewProjection, objLightMVP);
                static const float kZeroExtra[4] = {};
                const RhiGpuAddress cameraCbGpu = maskExtra0
                    ? m_frameCoordinator.PushCameraCB(*frame, objLightMVP, item.model, maskExtra0, kZeroExtra,
                                                      kZeroExtra, nullptr, nullptr, nullptr, maskExtra6)
                    : m_frameCoordinator.PushCameraCB(*frame, objLightMVP, item.model);
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
            };

            for (const auto& item : m_sceneSubmitter.GetSkinnedDrawItems()) {
                if (item.transparent) {
                    continue;
                }
                if (shadowContext.allowAlphaMaskPipeline && GetGBufferAlphaCutoff(item.material) > 0.0f) {
                    alphaMaskItems.push_back(&item);
                    continue;
                }
                drawSkinnedShadowMesh(item, nullptr, nullptr);
            }

            if (!alphaMaskItems.empty()) {
                auto& psoCache = m_pipelineStateCache;
                cmdList->SetPipelineState(shadowContext.d16Target
                                              ? psoCache.GetSkinnedShadowAlphaMaskD16PipelineState()
                                              : psoCache.GetSkinnedShadowAlphaMaskPipelineState());
                for (const SkinnedItem* maskItem : alphaMaskItems) {
                    const float extra6[4] = { 0.0f, 0.0f, GetGBufferAlphaCutoff(maskItem->material), 0.0f };
                    BindShadowAlphaMaskAlbedo(*enc, maskItem->texture);
                    drawSkinnedShadowMesh(*maskItem, maskItem->material.baseColor, extra6);
                }
                cmdList->SetPipelineState(shadowContext.d16Target ? psoCache.GetSkinnedShadowD16PipelineState()
                                                                  : psoCache.GetSkinnedShadowPipelineState());
            }
            // GPU crowd (VAT instancing) casts into the same map. It records the frame's pending
            // GPU step itself, so the shadow is not one step behind the Crowd pass.
            if (m_crowdRenderPass) {
                m_crowdRenderPass->DrawShadowDepth(graphicsCommandEncoder, m_frameCoordinator, *frame,
                                                   shadowContext.lightViewProjection, shadowContext.d16Target);
            }
        };

        context.executionServices = BuildRenderPassExecutionServices(
            context.drawItems,
            context.drawShadowItems,
            executionPolicy.useMeshShader);
        RenderPassExecutionServices& executionServices = context.executionServices;
        // Virtual-geometry cluster path (MeshShaderRenderPass, after OpaqueGBuffer): the pass
        // itself checks policy.useMeshShader, so the service is always provided.
        executionServices.drawClusterItems = [this, cmdList, frame, backIndex, &graphicsCommandEncoder]() {
            const ClusterDagGpuBuffer& clusterBuffer = m_sceneSubmitter.EnsureClusterDagGpuBuilt();

            // GPU-driven path: compute cluster selection + ExecuteIndirect mesh-shader dispatch,
            // switchable against the CPU-driven path below via render.clustergpumain. Falls back
            // automatically when any required subsystem is unavailable or the indirect GBuffer
            // PSO failed to build.
            const bool useGpuMainView = m_settings.clusterGpuMainView && m_settings.clusterGpuCulling &&
                m_clusterGpuCulling.IsValid() && m_clusterInstanceBuffer.IsValid() &&
                m_clusterMaterialTable.IsValid() && m_clusterPagePool.IsValid() &&
                m_pipelineStateCache.GetClusterIndirectGBufferPipelineState().Get();
            if (useGpuMainView) {
                // The GPU path draws only page-pool-resident clusters, so the main view must feed
                // its LOD requests into streaming; the CPU path reads the fully resident cluster
                // buffer and never needed this. Feedback here is from the frame that last used this
                // (backIndex, slot), i.e. GetBackBufferCount() frames old by construction.
                if (m_clusterGpuCulling.ReadFeedback(backIndex, ClusterGpuCulling::kMainViewSlot, m_clusterFeedbackScratch)) {
                    m_clusterStreamingFeedback.Accumulate(m_clusterFeedbackScratch);
                }

                ClusterMainIndirectInputs indirectIn{};
                indirectIn.encoder             = &graphicsCommandEncoder;
                indirectIn.cmdList             = cmdList;
                indirectIn.pipelineStateCache  = &m_pipelineStateCache;
                indirectIn.srvHeap             = m_srvAllocator.GetHeap();
                indirectIn.viewport            = &m_viewport;
                indirectIn.scissorRect         = &m_scissorRect;
                indirectIn.frameCoordinator    = &m_frameCoordinator;
                indirectIn.frame               = frame;
                indirectIn.camera              = &m_cameraState;
                indirectIn.culling             = &m_clusterGpuCulling;
                indirectIn.instances           = &m_clusterInstanceBuffer;
                indirectIn.materials           = &m_clusterMaterialTable;
                indirectIn.clusterBuffer       = &clusterBuffer;
                indirectIn.frameIndex          = backIndex;
                indirectIn.viewSlot            = ClusterGpuCulling::kMainViewSlot;
                indirectIn.lodThresholdPx      = m_settings.clusterLodThresholdPx;
                indirectIn.debugMode           = m_settings.clusterDebugMode;
                indirectIn.nullTextureSrvIndex = m_srvAllocator.GetIndex(m_nullTextureSrv);
                indirectIn.toonFace            = BuildClusterToonFaceShadow(m_lightSystem, m_toonFaceShadowMap, m_srvAllocator);
                indirectIn.pageOffsetTableVA   = m_clusterFrameUpdate.GetPageOffsetTableVA();
                indirectIn.pagePoolVA          = m_clusterPagePool.GetPoolVA();
                indirectIn.collectStats        = m_settings.clusterSelectStats;

                const ClusterDrawStats stats = ClusterDrawSubmitter::DrawMainIndirect(indirectIn);
                // itemsConsidered == 0 means nothing was recorded: fall through to the CPU path.
                if (stats.itemsConsidered != 0) {
                    m_lastClusterDrawStats = stats;
                    return;
                }
            }
            ClusterDrawInputs in{};
            in.encoder            = &graphicsCommandEncoder;
            in.cmdList            = cmdList;
            in.pipelineStateCache = &m_pipelineStateCache;
            in.srvHeap            = m_srvAllocator.GetHeap();
            in.viewport           = &m_viewport;
            in.scissorRect        = &m_scissorRect;
            in.frameCoordinator   = &m_frameCoordinator;
            in.frame              = frame;
            in.camera             = &m_cameraState;
            in.drawItems          = &m_sceneSubmitter.GetDrawItems();
            in.meshBuffer         = &m_meshBuffer;
            in.clusterBuffer      = &clusterBuffer;
            in.effectiveAoMode    = GetEffectiveAmbientOcclusionMode();
            in.defaultAlbedo      = m_defaultAlbedoTexture;
            in.defaultOcclusion   = m_defaultOcclusionTexture;
            in.defaultNormal      = m_defaultNormalTexture;
            in.defaultEmissive    = m_defaultEmissiveTexture;
            in.nullTextureSrv     = m_nullTextureSrv;
            in.toonFace           = BuildClusterToonFaceShadow(m_lightSystem, m_toonFaceShadowMap, m_srvAllocator);
            in.lodThresholdPx     = m_settings.clusterLodThresholdPx;
            in.debugMode          = m_settings.clusterDebugMode;
            m_lastClusterDrawStats = ClusterDrawSubmitter::Draw(in);
        };
        // Depth-only cluster path for the directional shadow cascades: invoked once per cascade
        // by LightSystem with the cascade DSV, viewport and scissor already set.
        executionServices.drawClusterShadowItems = [this, cmdList, frame, backIndex, &graphicsCommandEncoder, &executionPolicy](const LightSystem::ShadowPassContext& ctx) {
            const ClusterDagGpuBuffer& clusterBuffer = m_sceneSubmitter.EnsureClusterDagGpuBuilt();

            // cascadeTexelSize is computed against the SWRT shadow-map size, not the raster
            // cascade's, so texels-per-world-unit is derived directly from the light VP instead:
            // for the row-vector convention, column 1 (rows 0..2) is the NDC-y axis scaled by
            // 1 / orthoHalfHeight, so its length is the NDC-y unit scale per world unit, and
            // half that many NDC units span a texel row, hence * 0.5f * shadowMapHeight.
            const float* vp = ctx.lightViewProjection;
            const float ndcYPerWorldUnit = std::sqrt(vp[1] * vp[1] + vp[5] * vp[5] + vp[9] * vp[9]);
            // Real pixel size of the raster cascade maps (not the SWRT size the pass context is built with).
            const float shadowMapHeight = static_cast<float>(m_lightSystem.GetDirectionalShadowMapSize());
            const float texelsPerWorldUnit = ndcYPerWorldUnit * 0.5f * shadowMapHeight;

            // GPU-driven path: one compute cluster-selection dispatch + one ExecuteIndirect
            // mesh-shader dispatch per cascade, switchable against the CPU-driven path below
            // via render.clustergpucull. Falls back automatically when any of the required
            // subsystems failed to initialize or the indirect shadow PSO is unavailable.
            const bool useGpuCulling = m_settings.clusterGpuCulling && m_clusterGpuCulling.IsValid() &&
                m_clusterInstanceBuffer.IsValid() && m_clusterPagePool.IsValid() &&
                m_pipelineStateCache.GetClusterIndirectShadowPipelineState().Get();

            ClusterDrawStats cascadeStats{};
            if (useGpuCulling) {
                // The instance list, the resident set, the page-pool contents and the offset
                // table this cascade is about to read are all per-frame, not per-cascade, and
                // are prepared by Renderer::BeginClusterFrameUpdate before the graph runs
                // (RendererClusterFrame.cpp). They used to be prepared right here, which only
                // worked because the shadow pass happened to be the first GPU-driven view and
                // happened to run with GPU culling on.

                ClusterShadowIndirectInputs indirectIn{};
                indirectIn.cmdList             = cmdList;
                indirectIn.pipelineStateCache  = &m_pipelineStateCache;
                indirectIn.srvHeap             = m_srvAllocator.GetHeap();
                indirectIn.frameCoordinator    = &m_frameCoordinator;
                indirectIn.frame               = frame;
                indirectIn.culling             = &m_clusterGpuCulling;
                indirectIn.instances           = &m_clusterInstanceBuffer;
                indirectIn.clusterBuffer       = &clusterBuffer;
                indirectIn.frameIndex          = backIndex;
                indirectIn.viewSlot            = ctx.activeCascadeIndex;
                indirectIn.lightViewProjection = ctx.lightViewProjection;
                indirectIn.texelsPerWorldUnit  = texelsPerWorldUnit;
                indirectIn.lodThresholdPx      = m_settings.clusterShadowLodThresholdPx;
                indirectIn.pageOffsetTableVA   = m_clusterFrameUpdate.GetPageOffsetTableVA();
                indirectIn.pagePoolVA          = m_clusterPagePool.GetPoolVA();
                indirectIn.selectOnly          = m_settings.clusterShadowSelectOnly;
                indirectIn.collectStats        = m_settings.clusterSelectStats;

                // Feedback here is from whatever frame last used this (backIndex, viewSlot)
                // slot, i.e. GetBackBufferCount() frames old by construction: the frame
                // coordinator waits on that frame's fence before the slot is reused. This is
                // the usual latency of Nanite-style GPU-driven streaming feedback.
                if (m_clusterGpuCulling.ReadFeedback(backIndex, ctx.activeCascadeIndex, m_clusterFeedbackScratch)) {
                    m_clusterStreamingFeedback.Accumulate(m_clusterFeedbackScratch);
                }
                // Closing the feedback frame and re-deciding residency from it is per-frame
                // too: Renderer::EndClusterFrameUpdate does it once the graph has run, so it no
                // longer depends on this cascade being the last one of a directional-shadow pass.

                cascadeStats = ClusterDrawSubmitter::DrawShadowIndirect(indirectIn);
            } else {
                ClusterShadowDrawInputs shadowIn{};
                shadowIn.encoder             = &graphicsCommandEncoder;
                shadowIn.cmdList             = cmdList;
                shadowIn.pipelineStateCache  = &m_pipelineStateCache;
                shadowIn.frameCoordinator    = &m_frameCoordinator;
                shadowIn.frame               = frame;
                shadowIn.drawItems           = &m_sceneSubmitter.GetDrawItems();
                shadowIn.meshBuffer          = &m_meshBuffer;
                shadowIn.clusterBuffer       = &clusterBuffer;
                shadowIn.lightViewProjection = ctx.lightViewProjection;
                shadowIn.texelsPerWorldUnit  = texelsPerWorldUnit;
                shadowIn.lodThresholdPx      = m_settings.clusterShadowLodThresholdPx;

                cascadeStats = ClusterDrawSubmitter::DrawShadow(shadowIn);
            }

            if (ctx.activeCascadeIndex == 0u) {
                m_lastClusterShadowStats = cascadeStats;
            } else {
                m_lastClusterShadowStats.itemsConsidered   += cascadeStats.itemsConsidered;
                m_lastClusterShadowStats.itemsDrawn        += cascadeStats.itemsDrawn;
                m_lastClusterShadowStats.clustersDispatched += cascadeStats.clustersDispatched;
            }
        };
        if (!m_sceneSubmitter.GetSkinnedDrawItems().empty()) {
            executionServices.drawSkinnedOpaqueItems      = [drawSkinnedItems]() { drawSkinnedItems(false); };
            executionServices.drawSkinnedTransparentItems = [drawSkinnedItems]() { drawSkinnedItems(true); };
        }
        // The crowd also casts through this callback, so it must exist even with no skinned meshes.
        if (!m_sceneSubmitter.GetSkinnedDrawItems().empty() || m_crowdRenderPass) {
            executionServices.drawSkinnedShadowItems = drawSkinnedShadowItems;
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
        if (m_readyState.IsFeatureReady(m_readyState.smokeReady) && m_smokeSim.IsInitialized()) m_smokeSim.PrepareFrame(*m_device);
        if (m_readyState.IsFeatureReady(m_readyState.particlesReady) && m_particleSystem.IsInitialized()) m_particleSystem.PrepareFrame(*m_device);

        // Per-Node graphics command list rotation. Only engaged by RenderGraph::Execute()
        // at a genuine cross-queue join (a level with both a compute-preferred pass and an
        // available compute queue) — see the callbacks below. With zero passes preferring
        // the compute queue today, this path is never invoked and `cmdList` keeps pointing
        // at the single frame-long command list BeginFrame opened, so behaviour is unchanged.
        // extraGraphicsEncoders keeps each rotated node's encoder alive for the rest of the
        // frame, since frameInputs.execution.commandEncoder only stores a raw pointer to it.
        std::vector<std::unique_ptr<D3D12CommandListRhiEncoder>>& extraGraphicsEncoders = context.extraGraphicsEncoders;
        size_t& nextGraphicsNodeSlot = context.nextGraphicsNodeSlot;
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

        // context.executeContext is already default-constructed (FrameRenderContext is a
        // fresh per-frame local in Renderer::Render), matching the original's fresh
        // `RenderGraphExecuteContext executeContext{};` declaration here.
        if (m_skipShadowCasters) {
            // No-op, not null: the shadow passes still clear their maps, and null-guarded
            // paths (skinned, cluster) keep the state they would have without the flag.
            const auto drawNothing = [](const LightSystem::ShadowPassContext&) {};
            executionServices.drawShadowItems = drawNothing;
            if (executionServices.drawSkinnedShadowItems) {
                executionServices.drawSkinnedShadowItems = drawNothing;
            }
            if (executionServices.drawClusterShadowItems) {
                executionServices.drawClusterShadowItems = drawNothing;
            }
        }

        RenderGraphExecuteContext& executeContext = context.executeContext;
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
    }
}
