#include "Renderer/Crowd/CrowdVatGpuAsset.h"

#include "meshoptimizer/meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        constexpr uint32_t kFloat4Stride = 16;

        RhiBufferDesc MakeBufferDesc(uint64_t sizeInBytes, uint32_t strideInBytes,
                                     RhiBufferUsageFlags usage, RhiResourceState initialState)
        {
            RhiBufferDesc desc{};
            desc.sizeInBytes = sizeInBytes;
            desc.strideInBytes = strideInBytes;
            desc.usage = usage;
            desc.memoryUsage = RhiMemoryUsage::GpuOnly;
            desc.initialState = initialState;
            return desc;
        }

        RhiBufferHandle CreateTable(IRhiDevice& device, const void* data, uint64_t sizeInBytes, uint32_t strideInBytes)
        {
            return device.CreateRhiBuffer(
                MakeBufferDesc(sizeInBytes, strideInBytes,
                               RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                               RhiResourceState::ShaderResource),
                data);
        }
    }

    bool CrowdVatGpuAsset::Upload(IRhiDevice& device, const std::vector<SkinnedMesh>& meshes,
                                  const VatBakedAnimation& baked, std::string& outError)
    {
        uint64_t meshVertexTotal = 0;
        uint64_t meshIndexTotal = 0;
        for (const SkinnedMesh& mesh : meshes)
        {
            meshVertexTotal += mesh.vertices.size();
            // glTF: a primitive without indices is a non-indexed triangle list (0..N-1).
            meshIndexTotal += mesh.indices.empty() ? mesh.vertices.size() : mesh.indices.size();
        }

        if (baked.vertexCount == 0)
        {
            outError = "CrowdVatGpuAsset: the baked animation has no vertices";
            return false;
        }
        if (baked.clips.empty())
        {
            outError = "CrowdVatGpuAsset: the baked animation has no clips";
            return false;
        }
        if (meshVertexTotal != baked.vertexCount)
        {
            outError = "CrowdVatGpuAsset: the mesh vertex count differs from the baked vertex count";
            return false;
        }
        if (meshIndexTotal == 0)
        {
            outError = "CrowdVatGpuAsset: the meshes have no indices";
            return false;
        }

        const uint64_t vertexTableFloats = static_cast<uint64_t>(baked.totalFrames) * baked.vertexCount * 4;
        const uint64_t boneTableFloats = static_cast<uint64_t>(baked.totalFrames) * baked.boneCount * 12;
        if (baked.totalFrames == 0)
        {
            outError = "CrowdVatGpuAsset: the baked animation has no frames";
            return false;
        }
        if (baked.mode == VatMode::Vertex)
        {
            if (baked.positions.size() != vertexTableFloats || baked.normals.size() != vertexTableFloats)
            {
                outError = "CrowdVatGpuAsset: the baked vertex tables have the wrong size";
                return false;
            }
        }
        else
        {
            if (baked.boneCount == 0 || baked.boneRows.size() != boneTableFloats)
            {
                outError = "CrowdVatGpuAsset: the baked bone table has the wrong size";
                return false;
            }
        }

        Release();
        m_device = &device;

        std::vector<CrowdSkinVertexGpu> skinVertices;
        const void* tableAData = nullptr;
        uint64_t tableABytes = 0;
        const void* tableBData = nullptr;
        uint64_t tableBBytes = 0;
        uint32_t tableBStride = kFloat4Stride;
        if (baked.mode == VatMode::Vertex)
        {
            tableAData = baked.positions.data();
            tableABytes = vertexTableFloats * sizeof(float);
            tableBData = baked.normals.data();
            tableBBytes = vertexTableFloats * sizeof(float);
        }
        else
        {
            skinVertices.reserve(baked.vertexCount);
            for (const SkinnedMesh& mesh : meshes)
            {
                for (const SkinnedVertex& src : mesh.vertices)
                {
                    CrowdSkinVertexGpu dst{};
                    for (int i = 0; i < 3; ++i)
                    {
                        dst.position[i] = src.position[i];
                        dst.normal[i] = src.normal[i];
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        dst.boneIndices |= static_cast<uint32_t>(src.boneIndices[i]) << (8 * i);
                        dst.boneWeights[i] = src.boneWeights[i];
                    }
                    skinVertices.push_back(dst);
                }
            }
            tableAData = baked.boneRows.data();
            tableABytes = boneTableFloats * sizeof(float);
            tableBData = skinVertices.data();
            tableBBytes = static_cast<uint64_t>(skinVertices.size()) * sizeof(CrowdSkinVertexGpu);
            tableBStride = static_cast<uint32_t>(sizeof(CrowdSkinVertexGpu));
        }

        std::vector<CrowdClipGpu> clips(baked.clips.size());
        std::vector<float> clipDurations(baked.clips.size());
        for (size_t i = 0; i < baked.clips.size(); ++i)
        {
            clips[i].firstFrame = baked.clips[i].firstFrame;
            clips[i].frameCount = baked.clips[i].frameCount;
            clips[i].durationSec = baked.clips[i].durationSec;
            clipDurations[i] = baked.clips[i].durationSec;
        }

        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(meshIndexTotal));
        uint32_t vertexOffset = 0;
        for (const SkinnedMesh& mesh : meshes)
        {
            if (mesh.indices.empty())
            {
                for (uint32_t i = 0; i < static_cast<uint32_t>(mesh.vertices.size()); ++i)
                {
                    indices.push_back(i + vertexOffset);
                }
            }
            for (const uint32_t index : mesh.indices)
            {
                indices.push_back(index + vertexOffset);
            }
            vertexOffset += static_cast<uint32_t>(mesh.vertices.size());
        }

        // LODs: index-only simplification of LOD0 on the bind-pose positions
        // (meshoptimizer meshopt_simplify, zeux). Each LOD is simplified from LOD0, not chained.
        std::vector<float> positions;
        positions.reserve(static_cast<size_t>(meshVertexTotal) * 3);
        for (const SkinnedMesh& mesh : meshes)
        {
            for (const SkinnedVertex& src : mesh.vertices)
            {
                positions.push_back(src.position[0]);
                positions.push_back(src.position[1]);
                positions.push_back(src.position[2]);
            }
        }

        const size_t lod0Count = indices.size();
        CrowdLodRange lodRanges[CROWD_MAX_LODS]{};
        lodRanges[0] = { 0, static_cast<uint32_t>(lod0Count) };
        uint32_t lodCount = 1;
        const float lodRatios[CROWD_MAX_LODS - 1] = { 0.5f, 0.2f };
        const float lodErrors[CROWD_MAX_LODS - 1] = { 1e-2f, 5e-2f };
        std::vector<uint32_t> allIndices = indices;
        size_t previousCount = lod0Count;
        for (uint32_t lod = 1; lod < CROWD_MAX_LODS; ++lod)
        {
            const size_t targetCount = (std::max)(static_cast<size_t>(3),
                static_cast<size_t>(static_cast<double>(lod0Count) * lodRatios[lod - 1]) / 3 * 3);
            std::vector<uint32_t> simplified(lod0Count);
            float resultError = 0.0f;
            const size_t resultCount = meshopt_simplify(simplified.data(), indices.data(), lod0Count,
                                                        positions.data(), static_cast<size_t>(baked.vertexCount),
                                                        sizeof(float) * 3, targetCount, lodErrors[lod - 1], 0,
                                                        &resultError);
            if (resultCount == 0 || resultCount >= previousCount)
            {
                break;
            }
            simplified.resize(resultCount);
            lodRanges[lod] = { static_cast<uint32_t>(allIndices.size()), static_cast<uint32_t>(resultCount) };
            allIndices.insert(allIndices.end(), simplified.begin(), simplified.end());
            previousCount = resultCount;
            ++lodCount;
        }

        m_tableA = CreateTable(device, tableAData, tableABytes, kFloat4Stride);
        m_tableB = CreateTable(device, tableBData, tableBBytes, tableBStride);
        m_clipTable = CreateTable(device, clips.data(), clips.size() * sizeof(CrowdClipGpu),
                                  static_cast<uint32_t>(sizeof(CrowdClipGpu)));
        m_indexBuffer = device.CreateRhiBuffer(
            MakeBufferDesc(allIndices.size() * sizeof(uint32_t), sizeof(uint32_t),
                           RhiBufferUsageFlags::Index, RhiResourceState::Common),
            allIndices.data());
        if (!m_tableA.IsValid() || !m_tableB.IsValid() || !m_clipTable.IsValid() || !m_indexBuffer.IsValid())
        {
            Release();
            outError = "CrowdVatGpuAsset: failed to create the GPU buffers";
            return false;
        }

        m_tableAAddress = device.GetRhiBufferGpuAddress(m_tableA);
        m_tableBAddress = device.GetRhiBufferGpuAddress(m_tableB);
        m_clipAddress = device.GetRhiBufferGpuAddress(m_clipTable);
        if (m_tableAAddress == 0 || m_tableBAddress == 0 || m_clipAddress == 0)
        {
            Release();
            outError = "CrowdVatGpuAsset: GPU addresses unavailable on this backend";
            return false;
        }

        m_mode = baked.mode;
        m_vertexCount = baked.vertexCount;
        m_boneCount = baked.boneCount;
        m_indexCount = static_cast<uint32_t>(indices.size());
        m_totalIndexCount = static_cast<uint32_t>(allIndices.size());
        m_lodCount = lodCount;
        for (uint32_t i = 0; i < CROWD_MAX_LODS; ++i)
        {
            m_lods[i] = lodRanges[i];
        }
        m_clipDurations = std::move(clipDurations);
        for (int i = 0; i < 3; ++i)
        {
            m_boundsMin[i] = baked.boundsMin[i];
            m_boundsMax[i] = baked.boundsMax[i];
        }

        // Sphere centred on the Y axis so it holds the model under any heading about Y.
        m_sphereCenterY = (m_boundsMin[1] + m_boundsMax[1]) * 0.5f;
        const float halfY = (m_boundsMax[1] - m_boundsMin[1]) * 0.5f;
        const float rx = (std::max)(std::fabs(m_boundsMin[0]), std::fabs(m_boundsMax[0]));
        const float rz = (std::max)(std::fabs(m_boundsMin[2]), std::fabs(m_boundsMax[2]));
        m_sphereRadius = std::sqrt(rx * rx + rz * rz + halfY * halfY);
        return true;
    }

    void CrowdVatGpuAsset::Release()
    {
        if (m_device != nullptr)
        {
            for (const RhiBufferHandle buffer : { m_tableA, m_tableB, m_clipTable, m_indexBuffer })
            {
                if (buffer.IsValid())
                {
                    m_device->DestroyRhiResource(buffer);
                }
            }
        }
        m_device = nullptr;
        m_tableA = {};
        m_tableB = {};
        m_clipTable = {};
        m_indexBuffer = {};
        m_tableAAddress = 0;
        m_tableBAddress = 0;
        m_clipAddress = 0;
        m_mode = VatMode::Vertex;
        m_vertexCount = 0;
        m_boneCount = 0;
        m_indexCount = 0;
        m_totalIndexCount = 0;
        for (CrowdLodRange& lod : m_lods)
        {
            lod = {};
        }
        m_lodCount = 0;
        m_sphereCenterY = 0.0f;
        m_sphereRadius = 0.0f;
        m_clipDurations.clear();
        for (int i = 0; i < 3; ++i)
        {
            m_boundsMin[i] = 0.0f;
            m_boundsMax[i] = 0.0f;
        }
    }

    RhiIndexBufferBinding CrowdVatGpuAsset::IndexBinding() const
    {
        return { m_indexBuffer, 0, m_totalIndexCount * static_cast<uint32_t>(sizeof(uint32_t)), true };
    }
}
