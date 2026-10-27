#pragma once

#include "Renderer/Scene/SceneSubmitter.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // What changed between a captured draw-item list and the current one.
    struct DrawItemDiff
    {
        // The lists cannot be matched item by item (different count, an item's mesh index, mesh
        // vertex/index count or transparency changed, or an item lost its bounds). Callers must
        // redo the whole grid; changedItems and dirtyBounds are then empty.
        bool structural = false;
        // Indices (into the current list) of items whose model matrix or world bounds changed.
        std::vector<uint32_t> changedItems;
        // Per changed item: the union of its old and new world AABB, as min xyz then max xyz.
        std::vector<float> dirtyBounds;
    };

    // Per-item copy of what the voxelization of a static draw item depends on, kept so a later
    // update can find which items moved. Items are matched by position in the list, which holds
    // because SyncModelsToRenderer rebuilds proxies in the same ECS order on every sync.
    class DrawItemSnapshot
    {
    public:
        void Capture(const std::vector<SceneSubmitter::DrawItem>& drawItems, const std::vector<Mesh>& meshes);
        void Clear() { m_items.clear(); m_captured = false; }
        bool IsCaptured() const { return m_captured; }

        // Compares bit-for-bit (memcmp on the matrix and bounds), so any change, however small,
        // marks the item.
        DrawItemDiff Compare(const std::vector<SceneSubmitter::DrawItem>& drawItems, const std::vector<Mesh>& meshes) const;

    private:
        struct Item
        {
            size_t meshIndex = 0;
            size_t vertexCount = 0;
            size_t indexCount = 0;
            bool transparent = false;
            bool hasBounds = false;
            float model[16] = {};
            float boundsMin[3] = {};
            float boundsMax[3] = {};
        };
        std::vector<Item> m_items;
        bool m_captured = false;
    };
}

namespace SasamiEngine { using namespace Render; }
