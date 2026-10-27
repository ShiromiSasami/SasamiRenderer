#include "Renderer/Voxel/DrawItemSnapshot.h"

#include <algorithm>
#include <cstring>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        void MeshCounts(const std::vector<Mesh>& meshes, size_t meshIndex, size_t& outVertexCount, size_t& outIndexCount)
        {
            outVertexCount = 0;
            outIndexCount = 0;
            if (meshIndex < meshes.size())
            {
                outVertexCount = meshes[meshIndex].vertices.size();
                outIndexCount = meshes[meshIndex].indices.size();
            }
        }
    }

    void DrawItemSnapshot::Capture(const std::vector<SceneSubmitter::DrawItem>& drawItems, const std::vector<Mesh>& meshes)
    {
        m_items.clear();
        m_items.reserve(drawItems.size());
        for (const SceneSubmitter::DrawItem& drawItem : drawItems)
        {
            Item item;
            item.meshIndex = drawItem.meshIndex;
            MeshCounts(meshes, drawItem.meshIndex, item.vertexCount, item.indexCount);
            item.transparent = drawItem.transparent;
            item.hasBounds = drawItem.hasWorldBounds;
            std::memcpy(item.model, drawItem.model, sizeof(item.model));
            std::memcpy(item.boundsMin, drawItem.worldBoundsMin, sizeof(item.boundsMin));
            std::memcpy(item.boundsMax, drawItem.worldBoundsMax, sizeof(item.boundsMax));
            m_items.push_back(item);
        }
        m_captured = true;
    }

    DrawItemDiff DrawItemSnapshot::Compare(const std::vector<SceneSubmitter::DrawItem>& drawItems, const std::vector<Mesh>& meshes) const
    {
        DrawItemDiff diff;
        if (!m_captured || m_items.size() != drawItems.size())
        {
            diff.structural = true;
            return diff;
        }

        for (size_t i = 0; i < drawItems.size(); ++i)
        {
            const Item& old = m_items[i];
            const SceneSubmitter::DrawItem& cur = drawItems[i];
            size_t vertexCount = 0;
            size_t indexCount = 0;
            MeshCounts(meshes, cur.meshIndex, vertexCount, indexCount);
            if (old.meshIndex != cur.meshIndex || old.vertexCount != vertexCount || old.indexCount != indexCount ||
                old.transparent != cur.transparent || old.hasBounds != cur.hasWorldBounds)
            {
                diff.structural = true;
                diff.changedItems.clear();
                diff.dirtyBounds.clear();
                return diff;
            }
        }

        for (size_t i = 0; i < drawItems.size(); ++i)
        {
            const Item& old = m_items[i];
            const SceneSubmitter::DrawItem& cur = drawItems[i];
            const bool modelChanged = std::memcmp(old.model, cur.model, sizeof(old.model)) != 0;
            const bool boundsChanged = std::memcmp(old.boundsMin, cur.worldBoundsMin, sizeof(old.boundsMin)) != 0 ||
                                       std::memcmp(old.boundsMax, cur.worldBoundsMax, sizeof(old.boundsMax)) != 0;
            if (!modelChanged && !boundsChanged)
            {
                continue;
            }
            // Transparent items are not voxelized, so their motion does not matter.
            if (cur.transparent)
            {
                continue;
            }
            if (!cur.hasWorldBounds)
            {
                diff.structural = true;
                diff.changedItems.clear();
                diff.dirtyBounds.clear();
                return diff;
            }

            diff.changedItems.push_back(static_cast<uint32_t>(i));
            for (int axis = 0; axis < 3; ++axis)
            {
                diff.dirtyBounds.push_back((std::min)(old.boundsMin[axis], cur.worldBoundsMin[axis]));
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                diff.dirtyBounds.push_back((std::max)(old.boundsMax[axis], cur.worldBoundsMax[axis]));
            }
        }
        return diff;
    }
}
