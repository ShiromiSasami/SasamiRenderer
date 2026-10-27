#pragma once

#include "Renderer/Scene/SceneSubmitter.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Indexed world-space triangle soup, ready to feed VoxelizerMeshInput
    // (positionStrideInFloats = 3, indices = 3 per triangle).
    struct WorldTriangleSoup
    {
        std::vector<float> positions;    // xyz per vertex, world space
        std::vector<uint32_t> indices;   // 3 per triangle, into positions / 3
        float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
        bool hasBounds = false;          // false when no triangle was gathered
        uint32_t drawItemsUsed = 0;
        uint32_t drawItemsSkipped = 0;   // transparent, invalid mesh index or no triangles
    };

    // Transforms every opaque static draw item's mesh into world space (DrawItem::model,
    // row-vector convention with a w divide, as SceneSubmitter's culling bounds use) and
    // appends it to one soup. Transparent items and skinned meshes are not included.
    // Meshes without indices contribute their vertices as consecutive triangles.
    WorldTriangleSoup GatherOpaqueWorldTriangles(const std::vector<SceneSubmitter::DrawItem>& drawItems,
                                                 const std::vector<Mesh>& meshes);

    // Same, but only the items listed in itemIndices (indices into drawItems, in the order given;
    // out-of-range indices count as skipped). Uses the same transform code as the overload above,
    // so a vertex gathered here is bit-identical to the same vertex gathered there.
    WorldTriangleSoup GatherOpaqueWorldTriangles(const std::vector<SceneSubmitter::DrawItem>& drawItems,
                                                 const std::vector<Mesh>& meshes,
                                                 const std::vector<uint32_t>& itemIndices);
}

namespace SasamiEngine { using namespace Render; }
