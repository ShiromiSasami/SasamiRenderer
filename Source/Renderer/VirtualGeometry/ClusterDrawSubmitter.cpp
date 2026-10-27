#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterDrawSubmitter.h"

#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Culling/Frustum.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Scene/MaterialConstants.h"

#include <cstring>

namespace SasamiEngine::Render
{
    namespace
    {
        // CameraCB extra3..extra5 as MeshShader_Common.hlsli reads them (g_toonFace*). The map
        // index is a uint in HLSL, so its bit pattern is copied rather than converted.
        void PackToonFaceExtras(const ClusterToonFaceShadow& face, float out[3][4])
        {
            out[0][0] = face.toLight[0];
            out[0][1] = face.toLight[1];
            out[0][2] = face.toLight[2];
            out[0][3] = face.enabled ? 1.0f : 0.0f;
            out[1][0] = face.forward[0];
            out[1][1] = face.forward[1];
            out[1][2] = face.forward[2];
            out[1][3] = face.softness;
            out[2][0] = face.right[0];
            out[2][1] = face.right[1];
            out[2][2] = face.right[2];
            std::memcpy(&out[2][3], &face.mapSrvIndex, sizeof(uint32_t));
        }

        // Root [3]: b1, 36 dwords. Mirrors the HLSL DrawCBData layout exactly.
        struct DrawCBData
        {
            float model[16];
            float inverseModel[16];
            uint32_t clusterOffset;
            uint32_t clusterCount;
            uint32_t groupOffset;
            uint32_t pad;
        };
        static_assert(sizeof(DrawCBData) == 36 * sizeof(uint32_t));

        // Shared by Draw and DrawShadow: pipeline/root-signature binding, the optional
        // descriptor heap and viewport/scissor, and the root SRVs [0]/[9]/[10] carrying the
        // cluster/triangle/group description buffers, which are identical for every draw in
        // a pass. Viewport/scissor are left untouched when null (the shadow pass sets its own
        // cascade viewport/scissor before invoking DrawShadow).
        void BindClusterPass(IRhiCommandEncoder& encoder,
                              CommandList& cmdList,
                              RootSignature& rootSig,
                              PipelineState& pso,
                              DescriptorHeap* srvHeap,
                              const Viewport* viewport,
                              const Rect* scissorRect,
                              const ClusterDagGpuBuffer& clusterBuffer)
        {
            encoder.SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(rootSig));
            encoder.SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pso));
            if (srvHeap) {
                encoder.SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(*srvHeap));
            }
            if (viewport) {
                encoder.SetViewports(reinterpret_cast<const RhiViewport*>(viewport), 1);
            }
            if (scissorRect) {
                encoder.SetScissors(reinterpret_cast<const RhiRect*>(scissorRect), 1);
            }

            cmdList.SetGraphicsRootShaderResourceView(0, clusterBuffer.GetClusterDescGpuVA());
            cmdList.SetGraphicsRootShaderResourceView(9, clusterBuffer.GetClusterTriangleGpuVA());
            cmdList.SetGraphicsRootShaderResourceView(10, clusterBuffer.GetGroupDescGpuVA());
        }

        // Shared by Draw and DrawShadow: per-item vertex/cluster-vertex root SRVs [1]/[2], the
        // per-draw model/inverseModel/cluster-range root constants [3], and the DispatchMesh
        // call itself (one amplification-shader threadgroup per 64 clusters).
        void DispatchItemClusters(CommandList& cmdList,
                                   ID3D12GraphicsCommandList6& cmdList6,
                                   const ClusterDagGpuBuffer& clusterBuffer,
                                   D3D12_GPU_VIRTUAL_ADDRESS vertexBufferVA,
                                   const float model[16],
                                   const float inverseModel[16],
                                   const ClusterDagGpuBuffer::MeshRange& meshRange)
        {
            // The mesh shader reads the mesh vertex buffer as a StructuredBuffer through root SRV t1;
            // MeshBuffer resolves the address from its RHI handle (the legacy view is 0 there).
            cmdList.SetGraphicsRootShaderResourceView(1, vertexBufferVA);
            cmdList.SetGraphicsRootShaderResourceView(2, clusterBuffer.GetClusterVertexGpuVA());

            DrawCBData drawCb{};
            std::memcpy(drawCb.model, model, sizeof(drawCb.model));
            std::memcpy(drawCb.inverseModel, inverseModel, sizeof(drawCb.inverseModel));
            drawCb.clusterOffset = meshRange.clusterOffset;
            drawCb.clusterCount = meshRange.clusterCount;
            drawCb.groupOffset = meshRange.groupOffset;
            drawCb.pad = 0;
            cmdList6.SetGraphicsRoot32BitConstants(3, 36, &drawCb, 0);

            const UINT threadGroupCount = (meshRange.clusterCount + 63u) / 64u;
            cmdList6.DispatchMesh(threadGroupCount, 1, 1);
        }

        // Shared by Draw and DrawMainIndirect: the camera-derived values the main view feeds
        // to both the CameraCB and the LOD test.
        struct MainViewCamera
        {
            const float* camPos;
            const float* proj;
            float projScale; // projection scale in pixels: proj[5] * 0.5 * viewport height
            float nearClip;
        };

        MainViewCamera ComputeMainViewCamera(const CameraState& camera, const Viewport* viewport)
        {
            MainViewCamera result{};
            result.camPos = camera.GetPos();
            result.proj = camera.GetProj();
            const float viewportHeight = viewport ? viewport->Height : 0.0f;
            result.projScale = result.proj[5] * 0.5f * viewportHeight;
            result.nearClip = camera.GetNearClip();
            return result;
        }
    }

    ClusterDrawStats ClusterDrawSubmitter::Draw(const ClusterDrawInputs& in)
    {
        ClusterDrawStats stats{};

        if (!in.encoder || !in.cmdList || !in.pipelineStateCache || !in.frameCoordinator ||
            !in.frame || !in.camera || !in.drawItems || !in.meshBuffer || !in.clusterBuffer) {
            return stats;
        }

        PipelineState& pso = in.pipelineStateCache->GetMeshShaderPipelineState();
        RootSignature& rootSig = in.pipelineStateCache->GetMeshShaderRootSignature();
        if (!pso.Get() || !rootSig.Get() || !in.clusterBuffer->IsValid()) {
            return stats;
        }

        ComPtr<ID3D12GraphicsCommandList6> cmdList6;
        const HRESULT hr = in.cmdList->Get()->QueryInterface(IID_PPV_ARGS(&cmdList6));
        if (FAILED(hr) || !cmdList6) {
            static bool loggedOnce = false;
            if (!loggedOnce) {
                DebugLog("ClusterDrawSubmitter::Draw: ID3D12GraphicsCommandList6 not available.\n");
                loggedOnce = true;
            }
            return stats;
        }

        BindClusterPass(*in.encoder, *in.cmdList, rootSig, pso, in.srvHeap, in.viewport, in.scissorRect, *in.clusterBuffer);

        // One shared camera CB slot (root [4]) for the whole draw list; extra0/extra1 match the
        // CameraCB layout the amplification shader's CameraCBData expects (see MS_Meshlet/AS_Meshlet).
        const MainViewCamera view = ComputeMainViewCamera(*in.camera, in.viewport);
        const float* camPos = view.camPos;
        const float extra0[4] = { camPos[0], camPos[1], camPos[2], in.lodThresholdPx };
        // extra1.w is g_clusterDebugMode in MeshShader_Common.hlsli. Only the main view feeds it:
        // the shadow passes bind no pixel shader, so the value is unused there.
        const float extra1[4] = { view.projScale, view.nearClip, 0.0f,
                                  static_cast<float>(in.debugMode) };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // PushCameraCB needs extra0..extra2 at minimum
        float faceExtra[3][4];
        PackToonFaceExtras(in.toonFace, faceExtra);
        const RhiGpuAddress cameraCbGpu = in.frameCoordinator->PushCameraCB(
            *in.frame, in.camera->GetPV(), in.camera->GetProj(), extra0, extra1, extra2,
            faceExtra[0], faceExtra[1], faceExtra[2]);
        if (cameraCbGpu != 0) {
            in.encoder->SetGraphicsConstantBufferView(4, cameraCbGpu);
        }

        // Same frustum-cull rule as the VS OpaqueGBuffer loop: items without world bounds are
        // always kept (a false negative would make geometry vanish), transparent items are
        // skipped entirely (the cluster path only replaces the static opaque draw).
        const Culling::FrustumPlanes cameraFrustum = Culling::ExtractFrustumPlanes(in.camera->GetPV());
        std::vector<const SceneSubmitter::DrawItem*> drawList;
        drawList.reserve(in.drawItems->size());
        for (const auto& item : *in.drawItems) {
            if (item.transparent) {
                continue;
            }
            ++stats.itemsConsidered;
            if (item.hasWorldBounds &&
                !Culling::IsAabbVisible(cameraFrustum, item.worldBoundsMin, item.worldBoundsMax)) {
                continue;
            }
            drawList.push_back(&item);
        }

        const size_t meshRangeCount = in.clusterBuffer->GetMeshRangeCount();

        for (const SceneSubmitter::DrawItem* drawItem : drawList) {
            const auto& item = *drawItem;

            if (item.meshIndex >= meshRangeCount) {
                continue;
            }
            const ClusterDagGpuBuffer::MeshRange& meshRange = in.clusterBuffer->GetMeshRange(item.meshIndex);
            if (meshRange.clusterCount == 0) {
                continue;
            }
            // The mesh shader reads the mesh vertex buffer as a StructuredBuffer through root SRV t1;
            // MeshBuffer resolves the address from its RHI handle (the legacy view is 0 there).
            const D3D12_GPU_VIRTUAL_ADDRESS vertexBufferVA = in.meshBuffer->GetVertexBufferGpuVA(item.meshIndex);
            if (vertexBufferVA == 0) {
                continue;
            }

            const MaterialConstants mc = PackMaterialConstants(item, in.effectiveAoMode);
            const RhiGpuAddress materialCbGpu = in.frameCoordinator->PushCameraCB(
                *in.frame, item.model /* mvp unused by PS */, item.model,
                mc.extra[0], mc.extra[1], mc.extra[2], mc.extra[3], mc.extra[4], mc.extra[5], mc.extra[6]);
            if (materialCbGpu != 0) {
                in.encoder->SetGraphicsConstantBufferView(5, materialCbGpu);
            }

            if (item.texture) {
                in.encoder->SetGraphicsDescriptorTable(6, { item.texture->srv.ptr });
            } else if (in.defaultAlbedo) {
                in.encoder->SetGraphicsDescriptorTable(6, { in.defaultAlbedo->srv.ptr });
            } else {
                in.encoder->SetGraphicsDescriptorTable(6, { in.nullTextureSrv.ptr });
            }

            if (item.occlusionTexture) {
                in.encoder->SetGraphicsDescriptorTable(7, { item.occlusionTexture->srv.ptr });
            } else if (in.defaultOcclusion) {
                in.encoder->SetGraphicsDescriptorTable(7, { in.defaultOcclusion->srv.ptr });
            } else {
                in.encoder->SetGraphicsDescriptorTable(7, { in.nullTextureSrv.ptr });
            }

            if (item.normalTexture) {
                in.encoder->SetGraphicsDescriptorTable(8, { item.normalTexture->srv.ptr });
            } else if (in.defaultNormal) {
                in.encoder->SetGraphicsDescriptorTable(8, { in.defaultNormal->srv.ptr });
            } else {
                in.encoder->SetGraphicsDescriptorTable(8, { in.nullTextureSrv.ptr });
            }

            if (item.emissiveTexture) {
                in.encoder->SetGraphicsDescriptorTable(11, { item.emissiveTexture->srv.ptr });
            } else if (in.defaultEmissive) {
                in.encoder->SetGraphicsDescriptorTable(11, { in.defaultEmissive->srv.ptr });
            } else {
                in.encoder->SetGraphicsDescriptorTable(11, { in.nullTextureSrv.ptr });
            }
            // Same fallback chain as the VS path's face-map slot (RendererDrawCallbacks.cpp).
            if (in.toonFace.mapSrv.ptr != 0) {
                in.encoder->SetGraphicsDescriptorTable(12, { in.toonFace.mapSrv.ptr });
            } else if (in.defaultEmissive) {
                in.encoder->SetGraphicsDescriptorTable(12, { in.defaultEmissive->srv.ptr });
            } else {
                in.encoder->SetGraphicsDescriptorTable(12, { in.nullTextureSrv.ptr });
            }

            float inverseModel[16];
            if (!Foundation::Math::Invert4x4(item.model, inverseModel)) {
                // Degenerate (near-singular) model matrix: fall back to the model matrix itself
                // rather than propagate garbage into the normal-transform math downstream.
                std::memcpy(inverseModel, item.model, sizeof(inverseModel));
            }
            DispatchItemClusters(*in.cmdList, *cmdList6.Get(), *in.clusterBuffer, vertexBufferVA,
                                  item.model, inverseModel, meshRange);

            ++stats.itemsDrawn;
            stats.clustersDispatched += meshRange.clusterCount;
        }

        return stats;
    }

    ClusterDrawStats ClusterDrawSubmitter::DrawShadow(const ClusterShadowDrawInputs& in)
    {
        ClusterDrawStats stats{};

        if (!in.encoder || !in.cmdList || !in.pipelineStateCache || !in.frameCoordinator ||
            !in.frame || !in.drawItems || !in.meshBuffer || !in.clusterBuffer || !in.lightViewProjection) {
            return stats;
        }

        PipelineState& pso = in.pipelineStateCache->GetMeshShaderShadowPipelineState();
        RootSignature& rootSig = in.pipelineStateCache->GetMeshShaderRootSignature();
        if (!pso.Get() || !rootSig.Get() || !in.clusterBuffer->IsValid()) {
            return stats;
        }

        ComPtr<ID3D12GraphicsCommandList6> cmdList6;
        const HRESULT hr = in.cmdList->Get()->QueryInterface(IID_PPV_ARGS(&cmdList6));
        if (FAILED(hr) || !cmdList6) {
            static bool loggedOnce = false;
            if (!loggedOnce) {
                DebugLog("ClusterDrawSubmitter::DrawShadow: ID3D12GraphicsCommandList6 not available.\n");
                loggedOnce = true;
            }
            return stats;
        }

        // No descriptor heap or texture tables needed for a depth-only pass, and the caller has
        // already set the cascade's viewport/scissor, so both are left untouched here.
        BindClusterPass(*in.encoder, *in.cmdList, rootSig, pso, nullptr, nullptr, nullptr, *in.clusterBuffer);

        // extra1 mirrors the orthographic branch of the shared CameraCBData layout: x = texels
        // per world unit, z = orthographic flag. world (the model slot of the shared CB) is
        // unused by the shadow AS/MS and passed as identity for clarity.
        float identity[16];
        Foundation::Math::Identity4x4(identity);
        const float extra0[4] = { 0.0f, 0.0f, 0.0f, in.lodThresholdPx };
        const float extra1[4] = { in.texelsPerWorldUnit, 0.0f, 1.0f, 0.0f };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const RhiGpuAddress cameraCbGpu = in.frameCoordinator->PushCameraCB(
            *in.frame, in.lightViewProjection, identity, extra0, extra1, extra2);
        if (cameraCbGpu != 0) {
            in.encoder->SetGraphicsConstantBufferView(4, cameraCbGpu);
        }

        // Same per-cascade cull rule as drawShadowItems (RendererFrameGraph.cpp): the near plane
        // is ignored because an object between the light and the cascade box still casts a
        // shadow into it, and transparent items are skipped (this path only replaces the static
        // opaque shadow draw).
        const Culling::FrustumPlanes cascadeFrustum = Culling::ExtractFrustumPlanes(in.lightViewProjection);
        std::vector<const SceneSubmitter::DrawItem*> drawList;
        drawList.reserve(in.drawItems->size());
        for (const auto& item : *in.drawItems) {
            if (item.transparent) {
                continue;
            }
            ++stats.itemsConsidered;
            if (item.hasWorldBounds &&
                !Culling::IsAabbVisibleIgnoringNearPlane(cascadeFrustum, item.worldBoundsMin, item.worldBoundsMax)) {
                continue;
            }
            drawList.push_back(&item);
        }

        const size_t meshRangeCount = in.clusterBuffer->GetMeshRangeCount();

        for (const SceneSubmitter::DrawItem* drawItem : drawList) {
            const auto& item = *drawItem;

            if (item.meshIndex >= meshRangeCount) {
                continue;
            }
            const ClusterDagGpuBuffer::MeshRange& meshRange = in.clusterBuffer->GetMeshRange(item.meshIndex);
            if (meshRange.clusterCount == 0) {
                continue;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS vertexBufferVA = in.meshBuffer->GetVertexBufferGpuVA(item.meshIndex);
            if (vertexBufferVA == 0) {
                continue;
            }

            // The shadow AS/MS never reads the inverse-model matrix (no normal transform in a
            // depth-only pass), so the model matrix is passed again here to skip the Invert4x4
            // call the colour path needs.
            DispatchItemClusters(*in.cmdList, *cmdList6.Get(), *in.clusterBuffer, vertexBufferVA,
                                  item.model, item.model, meshRange);

            ++stats.itemsDrawn;
            stats.clustersDispatched += meshRange.clusterCount;
        }

        return stats;
    }

    ClusterDrawStats ClusterDrawSubmitter::DrawShadowIndirect(const ClusterShadowIndirectInputs& in)
    {
        ClusterDrawStats stats{};

        if (!in.cmdList || !in.pipelineStateCache || !in.frameCoordinator || !in.frame ||
            !in.culling || !in.instances || !in.clusterBuffer || !in.lightViewProjection) {
            return stats;
        }
        if (!in.culling->IsValid() || !in.instances->IsValid() || in.instances->GetInstanceCount() == 0 ||
            !in.clusterBuffer->IsValid()) {
            return stats;
        }
        if (in.pageOffsetTableVA == 0 || in.pagePoolVA == 0) {
            return stats;
        }

        RootSignature& rootSig = in.pipelineStateCache->GetClusterIndirectRootSignature();
        PipelineState& pso = in.pipelineStateCache->GetClusterIndirectShadowPipelineState();
        if (!rootSig.Get() || !pso.Get()) {
            return stats;
        }

        ComPtr<ID3D12GraphicsCommandList6> cmdList6;
        const HRESULT hr = in.cmdList->Get()->QueryInterface(IID_PPV_ARGS(&cmdList6));
        if (FAILED(hr) || !cmdList6) {
            static bool loggedOnce = false;
            if (!loggedOnce) {
                DebugLog("ClusterDrawSubmitter::DrawShadowIndirect: ID3D12GraphicsCommandList6 not available.\n");
                loggedOnce = true;
            }
            return stats;
        }

        // Same CameraCB layout as DrawShadow's orthographic branch: extra1.x is texels per
        // world unit, extra1.z is the orthographic flag. The model slot is unused by the
        // shadow AS/MS and passed as identity for clarity.
        float identity[16];
        Foundation::Math::Identity4x4(identity);
        const float extra0[4] = { 0.0f, 0.0f, 0.0f, in.lodThresholdPx };
        const float extra1[4] = { in.texelsPerWorldUnit, 0.0f, 1.0f, 0.0f };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const RhiGpuAddress cameraCbGpu = in.frameCoordinator->PushCameraCB(
            *in.frame, in.lightViewProjection, identity, extra0, extra1, extra2);

        const uint32_t instanceCount = in.instances->GetInstanceCount();

        ClusterSelectParams selectParams{};
        selectParams.viewProjection = in.lightViewProjection;
        selectParams.cameraPos[0] = 0.0f;
        selectParams.cameraPos[1] = 0.0f;
        selectParams.cameraPos[2] = 0.0f;
        selectParams.lodThresholdPx = in.lodThresholdPx;
        selectParams.projScale = in.texelsPerWorldUnit;
        selectParams.zNear = 0.0f;
        selectParams.orthographic = true;
        selectParams.instanceCount = instanceCount;
        selectParams.groupCount = in.clusterBuffer->GetTotalGroupCount();
        selectParams.pageOffsetTableVA = in.pageOffsetTableVA;
        selectParams.pagePoolVA = in.pagePoolVA;
        selectParams.collectStats = in.collectStats;

        in.culling->RecordSelect(*in.cmdList, in.frameIndex, in.viewSlot, selectParams,
                                  in.clusterBuffer->GetClusterDescGpuVA(),
                                  in.clusterBuffer->GetGroupDescGpuVA(),
                                  in.clusterBuffer->GetNodeDescGpuVA(),
                                  in.instances->GetGpuVA(in.frameIndex));

        if (in.srvHeap) {
            DescriptorHeap* heaps[] = { in.srvHeap };
            in.cmdList->SetDescriptorHeaps(1, heaps);
        }
        in.cmdList->SetGraphicsRootSignature(rootSig);
        in.cmdList->SetPipelineState(pso);
        if (cameraCbGpu != 0) {
            in.cmdList->SetGraphicsRootConstantBufferView(0, cameraCbGpu);
        }
        in.cmdList->SetGraphicsRootShaderResourceView(1, in.pagePoolVA);
        in.cmdList->SetGraphicsRootShaderResourceView(2, in.instances->GetGpuVA(in.frameIndex));
        in.cmdList->SetGraphicsRootShaderResourceView(3, in.culling->GetVisibleListVA(in.frameIndex, in.viewSlot));
        in.cmdList->SetGraphicsRootShaderResourceView(4, in.culling->GetCounterVA(in.frameIndex, in.viewSlot));

        // Everything above still runs when selectOnly is set -- the select dispatch, the root
        // bindings and the barriers -- so the difference this makes to the Shadow timing is the
        // cost of the draw alone.
        if (!in.selectOnly) {
            cmdList6->ExecuteIndirect(in.culling->GetDispatchMeshCommandSignature(), 1,
                                       in.culling->GetArgsResource(in.frameIndex, in.viewSlot), 0, nullptr, 0);
        }

        in.culling->RecordRelease(*in.cmdList, in.frameIndex, in.viewSlot);

        // The GPU-side visible-cluster count is not read back, so only the instance count
        // (the CPU-visible quantity) is reported; clustersDispatched stays 0 for this path.
        stats.itemsConsidered = instanceCount;
        stats.itemsDrawn = instanceCount;

        return stats;
    }

    // CONTRACT: when the returned stats.itemsConsidered == 0 this function has recorded NOTHING
    // on the command list (every early-out precedes the first cmdList call; PushCameraCB only
    // writes upload memory), and the caller falls back to the CPU-driven Draw.
    ClusterDrawStats ClusterDrawSubmitter::DrawMainIndirect(const ClusterMainIndirectInputs& in)
    {
        ClusterDrawStats stats{};

        if (!in.encoder || !in.cmdList || !in.pipelineStateCache || !in.srvHeap || !in.frameCoordinator ||
            !in.frame || !in.camera || !in.viewport || !in.scissorRect || !in.culling || !in.instances ||
            !in.materials || !in.clusterBuffer) {
            return stats;
        }
        if (!in.culling->IsValid() || !in.instances->IsValid() || in.instances->GetInstanceCount() == 0 ||
            !in.clusterBuffer->IsValid() || !in.materials->IsValid()) {
            return stats;
        }
        // The PS indexes the material table with the instance index; a short table would read garbage.
        if (in.materials->GetMaterialCount() < in.instances->GetInstanceCount()) {
            return stats;
        }
        if (in.pageOffsetTableVA == 0 || in.pagePoolVA == 0) {
            return stats;
        }

        RootSignature& rootSig = in.pipelineStateCache->GetClusterIndirectGBufferRootSignature();
        PipelineState& pso = in.pipelineStateCache->GetClusterIndirectGBufferPipelineState();
        if (!rootSig.Get() || !pso.Get()) {
            return stats;
        }

        ComPtr<ID3D12GraphicsCommandList6> cmdList6;
        const HRESULT hr = in.cmdList->Get()->QueryInterface(IID_PPV_ARGS(&cmdList6));
        if (FAILED(hr) || !cmdList6) {
            static bool loggedOnce = false;
            if (!loggedOnce) {
                DebugLog("ClusterDrawSubmitter::DrawMainIndirect: ID3D12GraphicsCommandList6 not available.\n");
                loggedOnce = true;
            }
            return stats;
        }

        const MainViewCamera view = ComputeMainViewCamera(*in.camera, in.viewport);
        const float* camPos = view.camPos;
        const float extra0[4] = { camPos[0], camPos[1], camPos[2], in.lodThresholdPx };
        const float extra1[4] = { view.projScale, view.nearClip, 0.0f, static_cast<float>(in.debugMode) };
        // extra2.x carries g_nullTextureSrvIndex, a uint in HLSL: store the bit pattern, not a converted value.
        float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        std::memcpy(&extra2[0], &in.nullTextureSrvIndex, sizeof(uint32_t));
        float faceExtra[3][4];
        PackToonFaceExtras(in.toonFace, faceExtra);
        const RhiGpuAddress cameraCbGpu = in.frameCoordinator->PushCameraCB(
            *in.frame, in.camera->GetPV(), in.camera->GetProj(), extra0, extra1, extra2,
            faceExtra[0], faceExtra[1], faceExtra[2]);

        const uint32_t instanceCount = in.instances->GetInstanceCount();

        ClusterSelectParams selectParams{};
        selectParams.viewProjection = in.camera->GetPV();
        selectParams.cameraPos[0] = camPos[0];
        selectParams.cameraPos[1] = camPos[1];
        selectParams.cameraPos[2] = camPos[2];
        selectParams.lodThresholdPx = in.lodThresholdPx;
        selectParams.projScale = view.projScale;
        selectParams.zNear = view.nearClip;
        selectParams.orthographic = false;
        selectParams.instanceCount = instanceCount;
        selectParams.groupCount = in.clusterBuffer->GetTotalGroupCount();
        selectParams.pageOffsetTableVA = in.pageOffsetTableVA;
        selectParams.pagePoolVA = in.pagePoolVA;
        selectParams.collectStats = in.collectStats;

        in.culling->RecordSelect(*in.cmdList, in.frameIndex, in.viewSlot, selectParams,
                                  in.clusterBuffer->GetClusterDescGpuVA(),
                                  in.clusterBuffer->GetGroupDescGpuVA(),
                                  in.clusterBuffer->GetNodeDescGpuVA(),
                                  in.instances->GetGpuVA(in.frameIndex));

        DescriptorHeap* heaps[] = { in.srvHeap };
        in.cmdList->SetDescriptorHeaps(1, heaps);
        in.encoder->SetViewports(reinterpret_cast<const RhiViewport*>(in.viewport), 1);
        in.encoder->SetScissors(reinterpret_cast<const RhiRect*>(in.scissorRect), 1);
        in.cmdList->SetGraphicsRootSignature(rootSig);
        in.cmdList->SetPipelineState(pso);
        if (cameraCbGpu != 0) {
            in.cmdList->SetGraphicsRootConstantBufferView(0, cameraCbGpu);
        }
        in.cmdList->SetGraphicsRootShaderResourceView(1, in.pagePoolVA);
        in.cmdList->SetGraphicsRootShaderResourceView(2, in.instances->GetGpuVA(in.frameIndex));
        in.cmdList->SetGraphicsRootShaderResourceView(3, in.culling->GetVisibleListVA(in.frameIndex, in.viewSlot));
        in.cmdList->SetGraphicsRootShaderResourceView(4, in.culling->GetCounterVA(in.frameIndex, in.viewSlot));
        in.cmdList->SetGraphicsRootShaderResourceView(5, in.materials->GetGpuVA(in.frameIndex));

        cmdList6->ExecuteIndirect(in.culling->GetDispatchMeshCommandSignature(), 1,
                                   in.culling->GetArgsResource(in.frameIndex, in.viewSlot), 0, nullptr, 0);

        in.culling->RecordRelease(*in.cmdList, in.frameIndex, in.viewSlot);

        // The GPU-side visible-cluster count is not read back, so only the instance count
        // (the CPU-visible quantity) is reported; clustersDispatched stays 0 for this path.
        stats.itemsConsidered = instanceCount;
        stats.itemsDrawn = instanceCount;

        return stats;
    }
}
