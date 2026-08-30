#define NOMINMAX
#include "Renderer/Scene/SceneSubmitter.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/SkinnedMeshBuffer.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include "Renderer/RayTracing/DxrRayTracer.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/Utilities/TextureMipChain.h"
#include "Renderer/Utilities/HashUtility.h"
#include "Renderer/Structures/Skeleton.h"

namespace SasamiRenderer
{
    namespace
    {
        uint64_t ComputeMeshGeometryHash(const Mesh& mesh)
        {
            uint64_t hash = 0ull;
            const uint64_t vertexCount = static_cast<uint64_t>(mesh.vertices.size());
            const uint64_t indexCount  = static_cast<uint64_t>(mesh.indices.size());
            HashBytes(hash, &vertexCount, sizeof(vertexCount));
            HashBytes(hash, &indexCount,  sizeof(indexCount));
            if (!mesh.vertices.empty()) {
                HashBytes(hash, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
            }
            if (!mesh.indices.empty()) {
                HashBytes(hash, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
            }
            return hash;
        }

        bool MeshGeometryEquals(const Mesh& a, const Mesh& b)
        {
            if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size()) {
                return false;
            }
            if (!a.vertices.empty() &&
                std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) != 0) {
                return false;
            }
            if (!a.indices.empty() &&
                std::memcmp(a.indices.data(), b.indices.data(), a.indices.size() * sizeof(uint32_t)) != 0) {
                return false;
            }
            return true;
        }

        void ComputeMeshBounds(const Mesh& mesh, float outMin[3], float outMax[3])
        {
            if (mesh.vertices.empty()) {
                outMin[0] = outMin[1] = outMin[2] = 0.0f;
                outMax[0] = outMax[1] = outMax[2] = 0.0f;
                return;
            }
            float minB[3] = { mesh.vertices[0].position[0], mesh.vertices[0].position[1], mesh.vertices[0].position[2] };
            float maxB[3] = { mesh.vertices[0].position[0], mesh.vertices[0].position[1], mesh.vertices[0].position[2] };
            for (const Vertex& v : mesh.vertices) {
                minB[0] = std::min(minB[0], v.position[0]);
                minB[1] = std::min(minB[1], v.position[1]);
                minB[2] = std::min(minB[2], v.position[2]);
                maxB[0] = std::max(maxB[0], v.position[0]);
                maxB[1] = std::max(maxB[1], v.position[1]);
                maxB[2] = std::max(maxB[2], v.position[2]);
            }
            std::memcpy(outMin, minB, sizeof(minB));
            std::memcpy(outMax, maxB, sizeof(maxB));
        }

        void TransformPoint(const float matrix[16], const float point[3], float outPoint[3])
        {
            const float w    = point[0] * matrix[3] + point[1] * matrix[7] + point[2] * matrix[11] + matrix[15];
            const float invW = (std::fabs(w) > 1e-6f) ? (1.0f / w) : 1.0f;
            outPoint[0] = (point[0] * matrix[0] + point[1] * matrix[4] + point[2] * matrix[8]  + matrix[12]) * invW;
            outPoint[1] = (point[0] * matrix[1] + point[1] * matrix[5] + point[2] * matrix[9]  + matrix[13]) * invW;
            outPoint[2] = (point[0] * matrix[2] + point[1] * matrix[6] + point[2] * matrix[10] + matrix[14]) * invW;
        }

        void TransformBounds(const float matrix[16],
                             const float localMin[3], const float localMax[3],
                             float outMin[3], float outMax[3])
        {
            float worldMin[3] = { std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max()  };
            float worldMax[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

            // Enumerate all 8 corners via bit-mask to avoid MSVC 2D array init with runtime values.
            for (int mask = 0; mask < 8; ++mask) {
                const float corner[3] = {
                    (mask & 1) ? localMax[0] : localMin[0],
                    (mask & 2) ? localMax[1] : localMin[1],
                    (mask & 4) ? localMax[2] : localMin[2],
                };
                float t[3] = {};
                TransformPoint(matrix, corner, t);
                worldMin[0] = std::min(worldMin[0], t[0]); worldMin[1] = std::min(worldMin[1], t[1]); worldMin[2] = std::min(worldMin[2], t[2]);
                worldMax[0] = std::max(worldMax[0], t[0]); worldMax[1] = std::max(worldMax[1], t[1]); worldMax[2] = std::max(worldMax[2], t[2]);
            }
            std::memcpy(outMin, worldMin, sizeof(worldMin));
            std::memcpy(outMax, worldMax, sizeof(worldMax));
        }

    }

    SceneSubmitter::~SceneSubmitter()
    {
        ReleaseTextures();
    }

    void SceneSubmitter::ReleaseTextures()
    {
        if (m_device) {
            for (auto& texture : m_sceneTextures) {
                if (texture && texture->rhiTexture.IsValid()) {
                    m_device->DestroyRhiResource(texture->rhiTexture);
                    texture->rhiTexture = {};
                }
            }
        }
        m_sceneTextures.clear();
        m_textureCache.clear();
    }

    void SceneSubmitter::Initialize(const InitParams& params)
    {
        m_device             = params.device;
        m_meshBuffer         = params.meshBuffer;
        m_skinnedMeshBuffer  = params.skinnedMeshBuffer;
        m_rayTracingScene    = params.rayTracingScene;
        m_dxrRayTracer       = params.dxrRayTracer;
        m_srvAllocFn         = params.srvAllocFn;
        m_srvIndexFn         = params.srvIndexFn;
        m_deferredUploadSink = params.deferredUploadSink;
    }

    Texture* SceneSubmitter::CreateTextureFromRgba8Data(const CpuTextureRgba8& src,
                                                        CommandList* cmdList,
                                                        std::vector<Resource>& uploads)
    {
        if (src.pixels.empty() || src.width == 0 || src.height == 0) {
            return nullptr;
        }

        // The backend RHI creation path below blocks with a per-texture WaitForGPU inside
        // CreateRhiTexture2DFromRgba8. When the caller supplied a batch command list
        // (D3D12 compatibility surface), prefer the batched staging path further down:
        // it executes once per submit and retires staging via fence instead of stalling.
        const bool preferBatchedUpload =
            cmdList != nullptr &&
            m_device && m_device->GetCapabilities().supportsD3D12CompatibilitySurface;

        if (m_device && !preferBatchedUpload &&
            m_device->GetCapabilities().supportsRhiResourceCreation &&
            m_device->GetCapabilities().supportsRhiDescriptorCreation) {
            RhiTextureHandle rhiTexture = m_device->CreateRhiTexture2DFromRgba8(src.width,
                                                                                src.height,
                                                                                src.pixels.data(),
                                                                                src.width * 4u);
            if (rhiTexture.IsValid()) {
                RhiCpuDescriptorHandle rhiCpu{};
                GpuDescriptorHandle gpu{};
                bool descriptorReady = false;

                if (m_device->GetCapabilities().supportsD3D12CompatibilitySurface) {
                    CpuDescriptorHandle cpu{};
                    descriptorReady = m_srvAllocFn && m_srvAllocFn(1, cpu, gpu);
                    rhiCpu = { cpu.ptr };
                } else {
                    RhiDescriptorAllocation allocation =
                        m_device->AllocateRhiDescriptors(RhiDescriptorHeapType::CbvSrvUav, 1, true);
                    descriptorReady = allocation.cpu.IsValid() && allocation.gpu.IsValid();
                    rhiCpu = allocation.cpu;
                    gpu = { allocation.gpu.ptr };
                }

                if (descriptorReady) {
                    RhiTextureViewDesc srvDesc{};
                    srvDesc.format = RhiFormat::R8G8B8A8UNorm;
                    srvDesc.dimension = RhiTextureViewDimension::Texture2D;
                    // The RHI texture creation API cannot yet accept mip levels, so this
                    // path remains single-mip. DX12 always uses the batched upload path.
                    srvDesc.mipLevelCount = 1;
                    srvDesc.arrayLayerCount = 1;
                    if (m_device->CreateRhiShaderResourceView(rhiTexture, srvDesc, rhiCpu)) {
                        auto texObj = std::make_unique<Texture>();
                        texObj->rhiTexture   = rhiTexture;
                        texObj->srv          = gpu;
                        texObj->desc.width   = src.width;
                        texObj->desc.height  = src.height;
                        texObj->desc.mips    = 1;
                        texObj->desc.format  = DXGI_FORMAT_R8G8B8A8_UNORM;

                        m_sceneTextures.push_back(std::move(texObj));
                        return m_sceneTextures.back().get();
                    }
                }
                m_device->DestroyRhiResource(rhiTexture);
            }
        }

        if (!cmdList) {
            return nullptr;
        }

        // Material textures are sampled as R8G8B8A8_UNORM without an sRGB decode
        // (OpaqueGBuffer_PS.hlsl), so the chain is filtered in stored space to match the
        // hardware's own bilinear filtering of those values.
        const TextureMipChain mipChain =
            TextureMipChainBuilder::BuildRgba8(src.pixels.data(), src.width, src.height, false);

        Resource texture;
        Resource upload;
        UINT uploadedMipCount = 1;
        if (mipChain.IsValid() &&
            ResourceUploadUtility::CreateTexture2DFromRgba8WithMips(*m_device,
                                                                    cmdList,
                                                                    mipChain,
                                                                    texture,
                                                                    upload)) {
            uploadedMipCount = mipChain.LevelCount();
        } else if (!ResourceUploadUtility::CreateTexture2DFromRgba8(*m_device,
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
        if (!m_srvAllocFn(1, cpu, gpu)) {
            return nullptr;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels     = uploadedMipCount;
        m_device->CreateShaderResourceView(texture, &srvDesc, cpu);

        auto texObj = std::make_unique<Texture>();
        texObj->resource     = texture;
        texObj->srv          = gpu;
        texObj->desc.width   = src.width;
        texObj->desc.height  = src.height;
        texObj->desc.mips    = uploadedMipCount;
        texObj->desc.format  = DXGI_FORMAT_R8G8B8A8_UNORM;

        uploads.push_back(upload);
        m_sceneTextures.push_back(std::move(texObj));
        return m_sceneTextures.back().get();
    }

    Texture* SceneSubmitter::ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData,
                                                 TextureUploadBatch& batch)
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

        Texture* texture = nullptr;
        if (m_device->GetCapabilities().supportsD3D12CompatibilitySurface) {
            if (!batch.open) {
                HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, batch.allocator);
                if (SUCCEEDED(hr)) {
                    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, batch.allocator, nullptr, batch.commandList);
                }
                if (FAILED(hr)) {
                    return nullptr;
                }
                batch.open = true;
            }
            texture = CreateTextureFromRgba8Data(*textureData, &batch.commandList, batch.uploads);
        } else {
            std::vector<Resource> uploads;
            texture = CreateTextureFromRgba8Data(*textureData, nullptr, uploads);
        }
        if (!texture) {
            return nullptr;
        }

        m_textureCache[textureId] = texture;
        return texture;
    }

    void SceneSubmitter::FlushTextureUploadBatch(TextureUploadBatch& batch)
    {
        if (!batch.open) {
            return;
        }

        batch.commandList->Close();
        if (batch.uploads.empty()) {
            // Nothing was recorded (e.g. every miss failed) — drop the batch silently.
            batch.open = false;
            return;
        }
        ID3D12CommandList* lists[] = { batch.commandList.Get() };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, lists);

        const size_t textureCount = batch.uploads.size();
        if (m_deferredUploadSink) {
            m_deferredUploadSink(std::move(batch.allocator), std::move(batch.commandList), std::move(batch.uploads));
        } else {
            m_device->WaitForGPU();
        }
        const std::string perfMessage = "[Perf] SceneSubmitter: texture upload batch: " +
                                        std::to_string(textureCount) + " textures, 1 submit\n";
        DebugLog(perfMessage.c_str());

        batch.open = false;
    }

    void SceneSubmitter::SubmitRenderProxies(std::vector<RenderProxy>&& proxies)
    {
        if (!m_device || proxies.empty()) {
            return;
        }

        std::unordered_map<uint64_t, std::vector<uint32_t>> rayTracingMeshBuckets;
        rayTracingMeshBuckets.reserve(m_rayTracingScene->meshes.size() + proxies.size());
        for (uint32_t existingMeshIndex = 0;
             existingMeshIndex < static_cast<uint32_t>(m_rayTracingScene->meshes.size());
             ++existingMeshIndex) {
            const uint64_t meshHash = ComputeMeshGeometryHash(m_rayTracingScene->meshes[existingMeshIndex].mesh);
            rayTracingMeshBuckets[meshHash].push_back(existingMeshIndex);
        }

        TextureUploadBatch textureUploadBatch;
        for (auto& proxy : proxies) {
            const size_t meshIndex = m_meshes.size();
            m_meshes.push_back(std::move(proxy.mesh));

            // Kept in lock-step with m_meshes (pushed once per mesh, same index) so the
            // ray-tracing mesh bounds below can reuse it instead of recomputing.
            MeshLocalBounds& localBounds = m_meshLocalBoundsCache.emplace_back();
            localBounds.valid = !m_meshes.back().vertices.empty();
            ComputeMeshBounds(m_meshes.back(), localBounds.min, localBounds.max);

            DrawItem item;
            item.meshIndex = meshIndex;
            Texture* resolvedAlbedoTexture = ResolveSceneTexture(proxy.albedoTexture, textureUploadBatch);
            item.texture = resolvedAlbedoTexture;
            if (!resolvedAlbedoTexture && proxy.albedoTexture) {
                DebugLog("SceneSubmitter::SubmitRenderProxies: failed to resolve albedo texture. White fallback is bound.\n");
            }
            item.occlusionTexture = ResolveSceneTexture(proxy.occlusionTexture, textureUploadBatch);
            item.normalTexture    = ResolveSceneTexture(proxy.normalTexture, textureUploadBatch);
            item.usesMetallicRoughnessTexture = proxy.usesMetallicRoughnessTexture;
            item.material    = proxy.material;
            item.transparent = proxy.transparent;
            item.debugLabel  = proxy.debugLabel;
            if (!item.occlusionTexture && proxy.occlusionTexture) {
                DebugLog("SceneSubmitter::SubmitRenderProxies: failed to resolve occlusion texture. AO fallback is bound.\n");
            }
            std::memcpy(item.model, proxy.model, sizeof(item.model));
            if (localBounds.valid) {
                TransformBounds(item.model, localBounds.min, localBounds.max,
                                item.worldBoundsMin, item.worldBoundsMax);
                item.hasWorldBounds = true;
            }
            m_drawItems.push_back(item);

            const uint64_t meshHash = ComputeMeshGeometryHash(m_meshes.back());
            uint32_t rayTracingMeshIndex = 0u;
            bool foundSharedMesh = false;
            auto bucketIt = rayTracingMeshBuckets.find(meshHash);
            if (bucketIt != rayTracingMeshBuckets.end()) {
                for (const uint32_t candidateMeshIndex : bucketIt->second) {
                    if (candidateMeshIndex >= m_rayTracingScene->meshes.size()) {
                        continue;
                    }
                    if (MeshGeometryEquals(m_rayTracingScene->meshes[candidateMeshIndex].mesh, m_meshes.back())) {
                        rayTracingMeshIndex = candidateMeshIndex;
                        foundSharedMesh     = true;
                        break;
                    }
                }
            }

            if (!foundSharedMesh) {
                rayTracingMeshIndex = static_cast<uint32_t>(m_rayTracingScene->meshes.size());
                RayTracingMesh rayTracingMesh{};
                rayTracingMesh.mesh = m_meshes.back();
                // Reuse the just-computed local bounds cache entry for this same mesh
                // (m_meshLocalBoundsCache stays in lock-step with m_meshes) instead of
                // walking its vertices a second time.
                std::memcpy(rayTracingMesh.localBoundsMin, localBounds.min, sizeof(rayTracingMesh.localBoundsMin));
                std::memcpy(rayTracingMesh.localBoundsMax, localBounds.max, sizeof(rayTracingMesh.localBoundsMax));
                m_rayTracingScene->meshes.push_back(std::move(rayTracingMesh));
                rayTracingMeshBuckets[meshHash].push_back(rayTracingMeshIndex);
            }

            RayTracingMaterial rayTracingMaterial{};
            rayTracingMaterial.albedoTexture           = proxy.albedoTexture;
            rayTracingMaterial.occlusionTexture         = proxy.occlusionTexture;
            rayTracingMaterial.material                 = proxy.material;
            rayTracingMaterial.albedoDescriptorIndex    = resolvedAlbedoTexture
                ? static_cast<int32_t>(m_srvIndexFn(resolvedAlbedoTexture->srv)) : -1;
            rayTracingMaterial.occlusionDescriptorIndex = item.occlusionTexture
                ? static_cast<int32_t>(m_srvIndexFn(item.occlusionTexture->srv)) : -1;
            const uint32_t materialIndex = static_cast<uint32_t>(m_rayTracingScene->materials.size());
            m_rayTracingScene->materials.push_back(std::move(rayTracingMaterial));

            RayTracingInstance rayTracingInstance{};
            rayTracingInstance.meshIndex     = rayTracingMeshIndex;
            rayTracingInstance.materialIndex = materialIndex;
            rayTracingInstance.transparent   = proxy.transparent;
            std::memcpy(rayTracingInstance.model, proxy.model, sizeof(rayTracingInstance.model));
            if (!Math::Invert4x4(proxy.model, rayTracingInstance.inverseModel)) {
                for (int i = 0; i < 16; ++i) {
                    rayTracingInstance.inverseModel[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                }
            }
            TransformBounds(proxy.model,
                            m_rayTracingScene->meshes[rayTracingMeshIndex].localBoundsMin,
                            m_rayTracingScene->meshes[rayTracingMeshIndex].localBoundsMax,
                            rayTracingInstance.worldBoundsMin,
                            rayTracingInstance.worldBoundsMax);
            m_rayTracingScene->instances.push_back(std::move(rayTracingInstance));
        }

        const size_t texturesUploaded = textureUploadBatch.uploads.size();
        FlushTextureUploadBatch(textureUploadBatch);

        m_meshBuffer->Upload(*m_device, m_meshes, m_deferredUploadSink ? &m_deferredUploadSink : nullptr);

        // Fence-deferred retirement lets small submits overlap with rendering, but a
        // bulk submit (a full scene streaming in) queues far more copy work than the
        // frame ring can outrun: the coordinator recycles a command allocator after N
        // frames, which then trips "allocator reset before previous executions
        // completed" and takes the device down. Draining once here costs a single stall
        // on the frame the scene lands and keeps the per-texture stalls gone.
        constexpr size_t kBulkUploadTextureThreshold = 64u;
        constexpr size_t kBulkUploadMeshThreshold = 256u;
        if (texturesUploaded >= kBulkUploadTextureThreshold || m_meshes.size() >= kBulkUploadMeshThreshold) {
            m_device->WaitForGPU();
        }

        ++m_rayTracingScene->geometryVersion;
        ++m_rayTracingScene->materialVersion;
        ++m_rayTracingScene->instanceVersion;
        // DXR acceleration structures are rebuilt lazily right before hardware RT is
        // dispatched (Renderer's executeRayTracing service), keyed off the version bumps
        // above. Rebuilding here would stall scene submission (WaitForGPU + full BLAS
        // rebuild) even when the DXR path is never used.

        // See EnsureMeshletsBuilt: meshlets are rebuilt lazily on first actual use of the
        // mesh-shader path, not here. Marking the flag is free; scenes that never touch
        // that path never pay the Build()/Upload() cost.
        m_meshletsDirty = true;
    }

    void SceneSubmitter::ClearSubmittedRenderProxies()
    {
        m_drawItems.clear();
        m_meshes.clear();
        m_meshLocalBoundsCache.clear();
        m_rayTracingScene->Clear();
        // See SubmitRenderProxies: DXR AS teardown/rebuild happens lazily at dispatch
        // time. A stale TLAS is harmless while hardware RT is not being dispatched.

        m_meshletBuffer.Release();
        m_meshletsDirty = true;
    }

    const MeshletBuffer& SceneSubmitter::EnsureMeshletsBuilt()
    {
        if (!m_meshletsDirty) {
            return m_meshletBuffer;
        }
        m_meshletsDirty = false;

        if (!m_device || m_meshes.empty()) {
            m_meshletBuffer.Release();
            return m_meshletBuffer;
        }

        // Build() is a linear pass over already-parsed indices/vertices (bounding-sphere
        // math only, no triangulation/dedup), so this is expected to be cheap relative to
        // model loading -- but it is still real work done exactly once per mesh-list
        // change, hence the timer.
        ScopedPerfTimer timer("SceneSubmitter::EnsureMeshletsBuilt");
        m_meshletBuffer.Build(m_meshes);
        m_meshletBuffer.Upload(*m_device);
        return m_meshletBuffer;
    }

    void SceneSubmitter::SubmitSkinnedRenderProxies(std::vector<SkinnedRenderProxy>&& proxies,
                                                    RendererFrameCoordinator* frameCoord,
                                                    RendererFrameCoordinator::FrameContext* frame)
    {
        if (!m_device || !m_skinnedMeshBuffer) return;
        if (proxies.empty()) {
            ClearSkinnedRenderProxies();
            return;
        }

        // Ensure enough bone CB slots for this frame's skinned objects (DX12 CB-upload path only)
        if (frameCoord && frame) {
            frameCoord->EnsureBoneBuffers(*frame, static_cast<UINT>(proxies.size()));
        }

        // Skinning happens on the GPU (bone CB), so the vertex/index data is
        // static per mesh set. Re-uploading every frame released buffers the
        // GPU was still reading (in-flight destroy -> device fault), so only
        // rebuild the GPU buffers when the mesh set identity actually changes.
        bool meshSetChanged = m_residentSkinnedMeshIds.size() != proxies.size();
        if (!meshSetChanged) {
            for (size_t i = 0; i < proxies.size(); ++i) {
                if (m_residentSkinnedMeshIds[i] != proxies[i].meshId) { meshSetChanged = true; break; }
            }
        }

        m_skinnedDrawItems.clear();
        m_skinnedDrawItems.reserve(proxies.size());
        if (meshSetChanged) {
            m_skinnedMeshes.clear();
            m_skinnedMeshes.reserve(proxies.size());
            m_residentSkinnedMeshIds.clear();
            m_residentSkinnedMeshIds.reserve(proxies.size());
        }

        TextureUploadBatch textureUploadBatch;
        for (auto& proxy : proxies) {
            if (meshSetChanged) {
                m_residentSkinnedMeshIds.push_back(proxy.meshId);
                m_skinnedMeshes.push_back(std::move(proxy.mesh));
            }

            SkinnedDrawItem item;
            item.meshIndex       = m_skinnedDrawItems.size();
            item.texture         = ResolveSceneTexture(proxy.albedoTexture, textureUploadBatch);
            item.occlusionTexture = ResolveSceneTexture(proxy.occlusionTexture, textureUploadBatch);
            item.normalTexture   = ResolveSceneTexture(proxy.normalTexture, textureUploadBatch);
            item.material        = proxy.material;
            item.transparent     = proxy.transparent;
            std::memcpy(item.model, proxy.model, sizeof(item.model));

            // Evaluate bone matrices, then either upload to the DX12 per-frame CB
            // or stash the raw array for a native backend to bind directly.
            if (proxy.animController && proxy.animController->HasSkeleton()) {
                float boneMatrices[Skeleton::kMaxBones * 16];
                proxy.animController->GetBoneMatrices(boneMatrices);
                if (frameCoord && frame) {
                    item.boneMatricesCbGpu = frameCoord->PushBoneCB(*frame, boneMatrices);
                } else {
                    item.boneMatricesNative.assign(boneMatrices, boneMatrices + Skeleton::kMaxBones * 16);
                }
            }

            m_skinnedDrawItems.push_back(item);
        }

        FlushTextureUploadBatch(textureUploadBatch);

        if (meshSetChanged) {
            // Replacing a live buffer set: wait for in-flight frames that may
            // still reference the old vertex/index buffers before Upload()
            // releases them. Rare (model load/unload only).
            if (!m_skinnedMeshBuffer->Items().empty()) {
                m_device->WaitForGPU();
            }
            m_skinnedMeshBuffer->Upload(*m_device, m_skinnedMeshes, m_deferredUploadSink ? &m_deferredUploadSink : nullptr);
        }
    }

    void SceneSubmitter::ClearSkinnedRenderProxies()
    {
        m_skinnedDrawItems.clear();
        m_skinnedMeshes.clear();
        m_residentSkinnedMeshIds.clear();
    }
}
