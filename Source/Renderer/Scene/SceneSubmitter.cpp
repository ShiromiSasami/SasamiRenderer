#define NOMINMAX
#include "Renderer/Scene/SceneSubmitter.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/SkinnedMeshBuffer.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include "Renderer/RayTracing/DxrRayTracer.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/Structures/Skeleton.h"

namespace SasamiRenderer
{
    namespace
    {
        void HashBytes(uint64_t& hash, const void* data, size_t size)
        {
            static constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
            static constexpr uint64_t kFnvPrime       = 1099511628211ull;
            if (hash == 0ull) {
                hash = kFnvOffsetBasis;
            }
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                hash ^= static_cast<uint64_t>(bytes[i]);
                hash *= kFnvPrime;
            }
        }

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

    void SceneSubmitter::Initialize(const InitParams& params)
    {
        m_device             = params.device;
        m_meshBuffer         = params.meshBuffer;
        m_skinnedMeshBuffer  = params.skinnedMeshBuffer;
        m_rayTracingScene    = params.rayTracingScene;
        m_dxrRayTracer       = params.dxrRayTracer;
        m_srvAllocFn         = params.srvAllocFn;
        m_srvIndexFn         = params.srvIndexFn;
    }

    Texture* SceneSubmitter::CreateTextureFromRgba8Data(const CpuTextureRgba8& src,
                                                        CommandList* cmdList,
                                                        std::vector<Resource>& uploads)
    {
        if (src.pixels.empty() || src.width == 0 || src.height == 0) {
            return nullptr;
        }

        if (m_device &&
            !m_device->GetCapabilities().supportsD3D12CompatibilitySurface &&
            m_device->GetCapabilities().supportsRhiResourceCreation &&
            m_device->GetCapabilities().supportsRhiDescriptorCreation) {
            RhiTextureHandle rhiTexture = m_device->CreateRhiTexture2DFromRgba8(src.width,
                                                                                src.height,
                                                                                src.pixels.data(),
                                                                                src.width * 4u);
            if (!rhiTexture.IsValid()) {
                return nullptr;
            }

            RhiDescriptorAllocation allocation =
                m_device->AllocateRhiDescriptors(RhiDescriptorHeapType::CbvSrvUav, 1, true);
            if (!allocation.cpu.IsValid() || !allocation.gpu.IsValid()) {
                return nullptr;
            }

            RhiTextureViewDesc srvDesc{};
            srvDesc.format = RhiFormat::R8G8B8A8UNorm;
            srvDesc.dimension = RhiTextureViewDimension::Texture2D;
            srvDesc.mipLevelCount = 1;
            srvDesc.arrayLayerCount = 1;
            if (!m_device->CreateRhiShaderResourceView(rhiTexture, srvDesc, allocation.cpu)) {
                return nullptr;
            }

            auto texObj = std::make_unique<Texture>();
            texObj->rhiTexture   = rhiTexture;
            texObj->srv          = { allocation.gpu.ptr };
            texObj->desc.width   = src.width;
            texObj->desc.height  = src.height;
            texObj->desc.mips    = 1;
            texObj->desc.format  = DXGI_FORMAT_R8G8B8A8_UNORM;

            m_sceneTextures.push_back(std::move(texObj));
            return m_sceneTextures.back().get();
        }

        if (!cmdList) {
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
        if (!m_srvAllocFn(1, cpu, gpu)) {
            return nullptr;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(texture, &srvDesc, cpu);

        auto texObj = std::make_unique<Texture>();
        texObj->resource     = texture;
        texObj->srv          = gpu;
        texObj->desc.width   = src.width;
        texObj->desc.height  = src.height;
        texObj->desc.mips    = 1;
        texObj->desc.format  = DXGI_FORMAT_R8G8B8A8_UNORM;

        uploads.push_back(upload);
        m_sceneTextures.push_back(std::move(texObj));
        return m_sceneTextures.back().get();
    }

    Texture* SceneSubmitter::ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData)
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

        std::vector<Resource> uploads;
        Texture* texture = nullptr;
        CommandAllocator uploadAlloc;
        CommandList      uploadList;
        HRESULT hr = S_OK;
        if (m_device->GetCapabilities().supportsD3D12CompatibilitySurface) {
            hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc);
            if (SUCCEEDED(hr)) {
                hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc, nullptr, uploadList);
            }
            if (FAILED(hr)) {
                return nullptr;
            }
            texture = CreateTextureFromRgba8Data(*textureData, &uploadList, uploads);
        } else {
            texture = CreateTextureFromRgba8Data(*textureData, nullptr, uploads);
        }
        if (!texture) {
            return nullptr;
        }

        if (m_device->GetCapabilities().supportsD3D12CompatibilitySurface) {
            uploadList->Close();
            ID3D12CommandList* lists[] = { uploadList.Get() };
            m_device->GetCommandQueue()->ExecuteCommandLists(1, lists);
            m_device->WaitForGPU();
            uploads.clear();
        }

        m_textureCache[textureId] = texture;
        return texture;
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

        for (auto& proxy : proxies) {
            const size_t meshIndex = m_meshes.size();
            m_meshes.push_back(std::move(proxy.mesh));

            DrawItem item;
            item.meshIndex = meshIndex;
            Texture* resolvedAlbedoTexture = ResolveSceneTexture(proxy.albedoTexture);
            item.texture = resolvedAlbedoTexture;
            if (!resolvedAlbedoTexture && proxy.albedoTexture) {
                DebugLog("SceneSubmitter::SubmitRenderProxies: failed to resolve albedo texture. White fallback is bound.\n");
            }
            item.occlusionTexture = ResolveSceneTexture(proxy.occlusionTexture);
            item.usesMetallicRoughnessTexture = proxy.usesMetallicRoughnessTexture;
            item.material    = proxy.material;
            item.transparent = proxy.transparent;
            item.debugLabel  = proxy.debugLabel;
            if (!item.occlusionTexture && proxy.occlusionTexture) {
                DebugLog("SceneSubmitter::SubmitRenderProxies: failed to resolve occlusion texture. AO fallback is bound.\n");
            }
            std::memcpy(item.model, proxy.model, sizeof(item.model));
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
                ComputeMeshBounds(rayTracingMesh.mesh, rayTracingMesh.localBoundsMin, rayTracingMesh.localBoundsMax);
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

        m_meshBuffer->Upload(*m_device, m_meshes);
        ++m_rayTracingScene->geometryVersion;
        ++m_rayTracingScene->materialVersion;
        ++m_rayTracingScene->instanceVersion;
        m_dxrRayTracer->UpdateScene(*m_device, *m_rayTracingScene);
    }

    void SceneSubmitter::ClearSubmittedRenderProxies()
    {
        m_drawItems.clear();
        m_meshes.clear();
        m_rayTracingScene->Clear();
        if (m_device) {
            m_dxrRayTracer->UpdateScene(*m_device, *m_rayTracingScene);
        }
    }

    void SceneSubmitter::SubmitSkinnedRenderProxies(std::vector<SkinnedRenderProxy>&& proxies,
                                                    RendererFrameCoordinator& frameCoord,
                                                    RendererFrameCoordinator::FrameContext& frame)
    {
        if (!m_device || !m_skinnedMeshBuffer) return;
        if (proxies.empty()) {
            ClearSkinnedRenderProxies();
            return;
        }

        // Ensure enough bone CB slots for this frame's skinned objects
        frameCoord.EnsureBoneBuffers(frame, static_cast<UINT>(proxies.size()));

        m_skinnedDrawItems.clear();
        m_skinnedMeshes.clear();
        m_skinnedMeshes.reserve(proxies.size());
        m_skinnedDrawItems.reserve(proxies.size());

        for (auto& proxy : proxies) {
            const size_t meshIndex = m_skinnedMeshes.size();
            m_skinnedMeshes.push_back(std::move(proxy.mesh));

            SkinnedDrawItem item;
            item.meshIndex       = meshIndex;
            item.texture         = ResolveSceneTexture(proxy.albedoTexture);
            item.occlusionTexture = ResolveSceneTexture(proxy.occlusionTexture);
            item.material        = proxy.material;
            item.transparent     = proxy.transparent;
            std::memcpy(item.model, proxy.model, sizeof(item.model));

            // Evaluate bone matrices and upload to per-frame CB
            if (proxy.animController && proxy.animController->HasSkeleton()) {
                float boneMatrices[Skeleton::kMaxBones * 16];
                proxy.animController->GetBoneMatrices(boneMatrices);
                item.boneMatricesCbGpu = frameCoord.PushBoneCB(frame, boneMatrices);
            }

            m_skinnedDrawItems.push_back(item);
        }

        m_skinnedMeshBuffer->Upload(*m_device, m_skinnedMeshes);
    }

    void SceneSubmitter::ClearSkinnedRenderProxies()
    {
        m_skinnedDrawItems.clear();
        m_skinnedMeshes.clear();
    }
}
