#define NOMINMAX
#include "Renderer/Scene/MeshletBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <windows.h>

#include "d3dx12.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void MeshletBuffer::Build(const std::vector<Mesh>& meshes)
    {
        m_meshletDescs.clear();
        m_meshletIndices.clear();
        m_meshRanges.clear();
        m_meshRanges.reserve(meshes.size());

        for (const Mesh& mesh : meshes)
        {
            MeshRange range{};
            range.meshletOffset = static_cast<uint32_t>(m_meshletDescs.size());
            range.meshletCount  = 0u;

            const uint32_t triCount = static_cast<uint32_t>(mesh.indices.size()) / 3u;
            uint32_t triOffset = 0;

            while (triOffset < triCount)
            {
                const uint32_t trisInMeshlet = std::min(kMaxTrianglesPerMeshlet, triCount - triOffset);

                // Compute bounding sphere via AABB (Ritter-style)
                float minB[3] = {  std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max() };
                float maxB[3] = { -std::numeric_limits<float>::max(),
                                  -std::numeric_limits<float>::max(),
                                  -std::numeric_limits<float>::max() };

                const uint32_t indexBase = triOffset * 3u;
                for (uint32_t t = 0; t < trisInMeshlet; ++t)
                {
                    for (uint32_t v = 0; v < 3u; ++v)
                    {
                        const uint32_t vi = mesh.indices[indexBase + t * 3u + v];
                        if (vi >= mesh.vertices.size()) continue;
                        const float* p = mesh.vertices[vi].position;
                        for (int c = 0; c < 3; ++c)
                        {
                            minB[c] = std::min(minB[c], p[c]);
                            maxB[c] = std::max(maxB[c], p[c]);
                        }
                    }
                }

                float cx = (minB[0] + maxB[0]) * 0.5f;
                float cy = (minB[1] + maxB[1]) * 0.5f;
                float cz = (minB[2] + maxB[2]) * 0.5f;
                float rx = (maxB[0] - minB[0]) * 0.5f;
                float ry = (maxB[1] - minB[1]) * 0.5f;
                float rz = (maxB[2] - minB[2]) * 0.5f;
                float radius = std::sqrt(rx * rx + ry * ry + rz * rz);

                MeshletDesc desc{};
                desc.indexOffset      = static_cast<uint32_t>(m_meshletIndices.size());
                desc.indexCount       = trisInMeshlet;
                desc.boundsCenter[0]  = cx;
                desc.boundsCenter[1]  = cy;
                desc.boundsCenter[2]  = cz;
                desc.boundsRadius     = radius;

                // Copy flat vertex indices for this meshlet (3 per triangle)
                for (uint32_t t = 0; t < trisInMeshlet; ++t)
                {
                    for (uint32_t v = 0; v < 3u; ++v)
                    {
                        m_meshletIndices.push_back(mesh.indices[indexBase + t * 3u + v]);
                    }
                }

                m_meshletDescs.push_back(desc);
                triOffset += trisInMeshlet;
                ++range.meshletCount;
            }

            m_meshRanges.push_back(range);
        }
    }

    bool MeshletBuffer::Upload(IRHIDevice& device)
    {
        m_descBuffer.Reset();
        m_indexBuffer.Reset();

        if (m_meshletDescs.empty())
        {
            return true;
        }

        // Helper: create a default GPU buffer and upload data via an upload heap
        auto uploadBuffer = [&](const void* data, size_t byteSize, Resource& outResource) -> bool
        {
            // Create upload heap buffer
            D3D12_HEAP_PROPERTIES uploadHeap{};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC uploadDesc{};
            uploadDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadDesc.Width              = static_cast<UINT64>(byteSize);
            uploadDesc.Height             = 1;
            uploadDesc.DepthOrArraySize   = 1;
            uploadDesc.MipLevels          = 1;
            uploadDesc.SampleDesc.Count   = 1;
            uploadDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            Resource uploadResource;
            HRESULT hr = device.CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                uploadResource);
            if (FAILED(hr))
            {
                DebugLog("MeshletBuffer::Upload: failed to create upload buffer.\n");
                return false;
            }

            // Map and copy
            void* pMapped = nullptr;
            hr = uploadResource->Map(0, nullptr, &pMapped);
            if (FAILED(hr))
            {
                DebugLog("MeshletBuffer::Upload: failed to map upload buffer.\n");
                return false;
            }
            std::memcpy(pMapped, data, byteSize);
            uploadResource->Unmap(0, nullptr);

            // For simplicity, keep as upload heap (GENERIC_READ state).
            // This is acceptable for read-only SRV data accessed from mesh/amplification shaders.
            // If default heap performance is needed, a staging copy would be required here.
            outResource = std::move(uploadResource);
            return true;
        };

        // Upload meshlet descriptor buffer
        const size_t descBytes = m_meshletDescs.size() * sizeof(MeshletDesc);
        if (!uploadBuffer(m_meshletDescs.data(), descBytes, m_descBuffer))
        {
            return false;
        }

        // Upload meshlet index buffer
        const size_t idxBytes = m_meshletIndices.size() * sizeof(uint32_t);
        if (!uploadBuffer(m_meshletIndices.data(), idxBytes, m_indexBuffer))
        {
            return false;
        }

        return true;
    }

    D3D12_GPU_VIRTUAL_ADDRESS MeshletBuffer::GetMeshletDescGpuVA() const
    {
        return m_descBuffer.IsValid() ? m_descBuffer.GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS MeshletBuffer::GetMeshletIndexGpuVA() const
    {
        return m_indexBuffer.IsValid() ? m_indexBuffer.GetGPUVirtualAddress() : 0u;
    }
}
