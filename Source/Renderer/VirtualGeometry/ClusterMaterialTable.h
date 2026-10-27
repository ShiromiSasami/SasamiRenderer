#pragma once
#include "Renderer/Resources/PerFrameGrowableUploadBuffer.h"
#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/Structures/RendererEnums.h"
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    class SrvDescriptorAllocator;
    class Texture;

    // Per-instance material for the GPU-driven cluster path, indexed by the same instance index as
    // GpuClusterInstance. extra[] is exactly PackMaterialConstants' extra0..extra6 (what the
    // CPU-driven cluster draw pushes through the CameraCB), and the four SRV fields are
    // shader-visible heap indices of the textures that path binds at root 6/7/8/11.
    struct GpuClusterMaterial
    {
        float    extra[7][4];
        uint32_t albedoSrv;      // kInvalidSrvIndex when neither the item nor the default has a texture
        uint32_t occlusionSrv;
        uint32_t normalSrv;
        uint32_t flags;          // ClusterMaterialTable::kFlag*
        uint32_t emissiveSrv;
        uint32_t pad[3];
    };
    static_assert(sizeof(GpuClusterMaterial) == 144);

    // The textures the CPU-driven cluster draw falls back to when a draw item has none.
    struct ClusterMaterialDefaults
    {
        const Texture* albedo    = nullptr;
        const Texture* occlusion = nullptr;
        const Texture* normal    = nullptr;
        const Texture* emissive  = nullptr;
    };

    // Builds the per-frame GpuClusterMaterial array that parallels ClusterInstanceBuffer's instance
    // list, so a GPU-driven main-view draw can fetch a material by instance index instead of
    // having it pushed per draw. Stage 5a only builds it; no shader reads it yet.
    class ClusterMaterialTable
    {
    public:
        static constexpr uint32_t kInvalidSrvIndex = 0xffffffffu;
        // Set when the item supplied its own texture, as opposed to a default.
        static constexpr uint32_t kFlagItemAlbedo    = 1u << 0;
        static constexpr uint32_t kFlagItemOcclusion = 1u << 1;
        static constexpr uint32_t kFlagItemNormal    = 1u << 2;
        static constexpr uint32_t kFlagItemEmissive  = 1u << 3;

        bool Initialize(GraphicsDevice& device, uint32_t frameCount, uint32_t initialCapacity);
        void Release();
        bool IsValid() const;

        // Writes one GpuClusterMaterial per entry of sourceItemIndices (ClusterInstanceBuffer::
        // GetSourceItemIndices), in that order, into slot frameIndex. Returns the number written.
        uint32_t Update(uint32_t frameIndex,
                        const std::vector<SceneSubmitter::DrawItem>& items,
                        const std::vector<uint32_t>& sourceItemIndices,
                        const SrvDescriptorAllocator& allocator,
                        const ClusterMaterialDefaults& defaults,
                        RendererEnums::AmbientOcclusionMode effectiveAoMode);

        D3D12_GPU_VIRTUAL_ADDRESS GetGpuVA(uint32_t frameIndex) const;
        uint32_t GetMaterialCount() const { return m_materialCount; }
        bool HasOverflowed() const { return m_overflowed; }

    private:
        PerFrameGrowableUploadBuffer m_buffer;
        uint32_t m_materialCount = 0;
        bool     m_overflowed    = false;
    };
}

namespace SasamiEngine { using namespace Render; }
