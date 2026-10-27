#pragma once

#include "Renderer/Crowd/VatAnimation.h"
#include "Renderer/RHI/RhiDevice.h"

#include <cstdint>
#include <string>
#include <vector>

#define CROWD_MAX_LODS 3

namespace SasamiEngine::Render::Crowd
{
    // One row of the clip table the crowd VS reads (StructuredBuffer<CrowdClipGpu>, t3).
    struct CrowdClipGpu
    {
        uint32_t firstFrame = 0;
        uint32_t frameCount = 0;
        float    durationSec = 0.0f;
        uint32_t pad = 0;
    };
    static_assert(sizeof(CrowdClipGpu) == 16, "must match Shaders/Raster/Geometry/Crowd/CrowdVat_VS.hlsl");

    // Bone mode only: the bind-pose vertex the VS skins with the baked bone rows (t1).
    // boneIndices packs SkinnedVertex::boneIndices[0..3] as bytes, index 0 in the low byte.
    struct CrowdSkinVertexGpu
    {
        float    position[3] = { 0.0f, 0.0f, 0.0f };
        uint32_t boneIndices = 0;
        float    normal[3] = { 0.0f, 0.0f, 0.0f };
        float    pad = 0.0f;
        float    boneWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };
    static_assert(sizeof(CrowdSkinVertexGpu) == 48, "must match Shaders/Raster/Geometry/Crowd/CrowdVat_VS.hlsl");

    // One level of detail inside the shared index buffer (TODO stage 8d). LOD0 is the source mesh.
    struct CrowdLodRange
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
    };

    // GPU copy of one baked VAT asset: the tables plus the index buffer the instanced draw uses.
    // Root SRV slots are fixed and their meaning depends on the bake mode:
    //   Vertex mode: tableA = positions (float4 per frame x vertex), tableB = normals (same layout).
    //   Bone mode:   tableA = bone rows (3 float4 per frame x bone),  tableB = CrowdSkinVertexGpu per vertex.
    // Every buffer is GpuOnly, created in ShaderResource (index buffer: IndexBuffer) and never
    // changes state, so the pass binds them without barriers. DX12 only for now: on a backend
    // whose GetRhiBufferGpuAddress returns 0, Upload fails.
    class CrowdVatGpuAsset
    {
    public:
        // Uploads `baked` and the concatenated index list of `meshes` (mesh order, each mesh's
        // indices offset by the vertices of the meshes before it, matching the VAT vertex order).
        // Replaces a previous upload. The caller must make sure the GPU is no longer reading it.
        // Also builds up to CROWD_MAX_LODS index-only LODs with meshopt_simplify on the bind-pose
        // positions (LOD1 ~50%, LOD2 ~20% of the indices; zeux/meshoptimizer README "Simplification").
        // The LODs share the VAT vertices, so only the index list changes; they are concatenated
        // into the one index buffer and described by Lod(i).
        bool Upload(IRhiDevice& device, const std::vector<SkinnedMesh>& meshes,
                    const VatBakedAnimation& baked, std::string& outError);
        void Release();

        bool IsValid() const { return m_indexCount != 0; }
        VatMode  Mode() const { return m_mode; }
        uint32_t VertexCount() const { return m_vertexCount; }
        uint32_t BoneCount() const { return m_boneCount; }
        // LOD0 index count.
        uint32_t IndexCount() const { return m_indexCount; }
        uint32_t LodCount() const { return m_lodCount; }
        // i < LodCount(); out-of-range returns LOD0.
        CrowdLodRange Lod(uint32_t i) const { return (i < m_lodCount) ? m_lods[i] : m_lods[0]; }
        // Unscaled model-space sphere around the Y axis that contains the baked bounds under any
        // heading rotation: centre (0, centerY, 0). Used by the GPU culler.
        float SphereCenterY() const { return m_sphereCenterY; }
        float SphereRadius() const { return m_sphereRadius; }
        uint32_t ClipCount() const { return static_cast<uint32_t>(m_clipDurations.size()); }
        const std::vector<float>& ClipDurations() const { return m_clipDurations; }
        const float* BoundsMin() const { return m_boundsMin; }
        const float* BoundsMax() const { return m_boundsMax; }

        RhiGpuAddress TableAAddress() const { return m_tableAAddress; }
        RhiGpuAddress TableBAddress() const { return m_tableBAddress; }
        RhiGpuAddress ClipTableAddress() const { return m_clipAddress; }
        RhiIndexBufferBinding IndexBinding() const;

    private:
        IRhiDevice* m_device = nullptr;
        RhiBufferHandle m_tableA{};
        RhiBufferHandle m_tableB{};
        RhiBufferHandle m_clipTable{};
        RhiBufferHandle m_indexBuffer{};
        RhiGpuAddress m_tableAAddress = 0;
        RhiGpuAddress m_tableBAddress = 0;
        RhiGpuAddress m_clipAddress = 0;
        VatMode  m_mode = VatMode::Vertex;
        uint32_t m_vertexCount = 0;
        uint32_t m_boneCount = 0;
        uint32_t m_indexCount = 0;
        uint32_t m_totalIndexCount = 0; // all LODs; IndexBinding() covers this many
        CrowdLodRange m_lods[CROWD_MAX_LODS]{};
        uint32_t m_lodCount = 0;
        float m_sphereCenterY = 0.0f;
        float m_sphereRadius = 0.0f;
        std::vector<float> m_clipDurations;
        float m_boundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float m_boundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}

namespace SasamiEngine { using namespace Render; }
