#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterMaterialTable.h"

#include "Renderer/Resources/SrvDescriptorAllocator.h"
#include "Renderer/Structures/Texture.h"
#include "Renderer/Scene/MaterialConstants.h"
#include "Renderer/VirtualGeometry/ClusterInstanceBuffer.h"
#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace SasamiEngine::Render
{
    namespace
    {
        // Mirrors the root 6/7/8 choice in ClusterDrawSubmitter::Draw: the item's own texture,
        // else the default, else nothing. Returns true when the item supplied its own.
        bool ResolveSrvIndex(const Texture* itemTexture,
                             const Texture* defaultTexture,
                             const SrvDescriptorAllocator& allocator,
                             uint32_t& outIndex)
        {
            outIndex = ClusterMaterialTable::kInvalidSrvIndex;
            if (itemTexture && itemTexture->srv.ptr != 0) {
                outIndex = allocator.GetIndex(itemTexture->srv);
                return true;
            }
            if (defaultTexture && defaultTexture->srv.ptr != 0) {
                outIndex = allocator.GetIndex(defaultTexture->srv);
            }
            return false;
        }
    }

    bool ClusterMaterialTable::Initialize(GraphicsDevice& device, uint32_t frameCount, uint32_t initialCapacity)
    {
        Release();
        return m_buffer.Initialize(device, frameCount, initialCapacity, sizeof(GpuClusterMaterial),
                                   ClusterInstanceBuffer::kMaxInstances, "ClusterMaterialTable");
    }

    void ClusterMaterialTable::Release()
    {
        m_buffer.Release();
        m_materialCount = 0;
        m_overflowed = false;
    }

    bool ClusterMaterialTable::IsValid() const
    {
        return m_buffer.IsValid();
    }

    uint32_t ClusterMaterialTable::Update(uint32_t frameIndex,
                                          const std::vector<SceneSubmitter::DrawItem>& items,
                                          const std::vector<uint32_t>& sourceItemIndices,
                                          const SrvDescriptorAllocator& allocator,
                                          const ClusterMaterialDefaults& defaults,
                                          RendererEnums::AmbientOcclusionMode effectiveAoMode)
    {
        m_materialCount = 0;
        m_overflowed = false;
        if (!IsValid() || frameIndex >= m_buffer.GetFrameCount()) {
            return 0;
        }

        m_buffer.TickRetired();

        const uint32_t required = static_cast<uint32_t>(sourceItemIndices.size());
        if (required > m_buffer.GetSlotCapacity(frameIndex) && !m_buffer.EnsureCapacity(frameIndex, required)) {
            m_overflowed = true;
            char message[256];
            std::snprintf(message, sizeof(message),
                          "[ClusterMaterialTable] Update: %u materials requested but the slot holds %u; "
                          "the instances past that index have no material.\n",
                          required, m_buffer.GetSlotCapacity(frameIndex));
            DebugLog(message);
        }

        uint8_t* mapped = m_buffer.GetMapped(frameIndex);
        if (!mapped) {
            return 0;
        }

        // Indexed by instance index, parallel to ClusterInstanceBuffer: entry i describes instance i,
        // so an entry is always written (zeroed when its draw item is gone) and never skipped.
        const uint32_t count = std::min(required, m_buffer.GetSlotCapacity(frameIndex));
        for (uint32_t i = 0; i < count; ++i) {
            GpuClusterMaterial material{};
            material.albedoSrv = kInvalidSrvIndex;
            material.occlusionSrv = kInvalidSrvIndex;
            material.normalSrv = kInvalidSrvIndex;
            material.emissiveSrv = kInvalidSrvIndex;
            material.pad[0] = material.pad[1] = material.pad[2] = 0;

            const uint32_t itemIndex = sourceItemIndices[i];
            if (itemIndex < items.size()) {
                const SceneSubmitter::DrawItem& item = items[itemIndex];
                const MaterialConstants mc = PackMaterialConstants(item, effectiveAoMode);
                std::memcpy(material.extra, mc.extra, sizeof(material.extra));

                if (ResolveSrvIndex(item.texture, defaults.albedo, allocator, material.albedoSrv)) {
                    material.flags |= kFlagItemAlbedo;
                }
                if (ResolveSrvIndex(item.occlusionTexture, defaults.occlusion, allocator, material.occlusionSrv)) {
                    material.flags |= kFlagItemOcclusion;
                }
                if (ResolveSrvIndex(item.normalTexture, defaults.normal, allocator, material.normalSrv)) {
                    material.flags |= kFlagItemNormal;
                }
                if (ResolveSrvIndex(item.emissiveTexture, defaults.emissive, allocator, material.emissiveSrv)) {
                    material.flags |= kFlagItemEmissive;
                }
            }
            // Upload heap is write-combined: compose locally, then one sequential copy.
            std::memcpy(mapped + static_cast<size_t>(i) * sizeof(GpuClusterMaterial), &material, sizeof(material));
        }

        m_materialCount = count;
        return count;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterMaterialTable::GetGpuVA(uint32_t frameIndex) const
    {
        return m_buffer.GetGpuVA(frameIndex);
    }
}
