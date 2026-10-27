#include "Renderer/Voxel/WorldTriangleGather.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        void TransformPoint(const float matrix[16], const float point[3], float outPoint[3])
        {
            const float w    = point[0] * matrix[3] + point[1] * matrix[7] + point[2] * matrix[11] + matrix[15];
            const float invW = (std::fabs(w) > 1e-6f) ? (1.0f / w) : 1.0f;
            outPoint[0] = (point[0] * matrix[0] + point[1] * matrix[4] + point[2] * matrix[8]  + matrix[12]) * invW;
            outPoint[1] = (point[0] * matrix[1] + point[1] * matrix[5] + point[2] * matrix[9]  + matrix[13]) * invW;
            outPoint[2] = (point[0] * matrix[2] + point[1] * matrix[6] + point[2] * matrix[10] + matrix[14]) * invW;
        }

        size_t TriangleCount(const Mesh& mesh)
        {
            return mesh.HasIndices() ? mesh.indices.size() / 3 : mesh.vertices.size() / 3;
        }

        bool IsGatherable(const SceneSubmitter::DrawItem& item, const std::vector<Mesh>& meshes)
        {
            return !item.transparent && item.meshIndex < meshes.size() && TriangleCount(meshes[item.meshIndex]) > 0;
        }
    }

    namespace
    {
        void AppendDrawItem(WorldTriangleSoup& soup,
                            const SceneSubmitter::DrawItem& item,
                            const std::vector<Mesh>& meshes,
                            bool& overflowed)
        {
            const size_t maxVertices = (std::numeric_limits<uint32_t>::max)();

            if (overflowed || !IsGatherable(item, meshes))
            {
                ++soup.drawItemsSkipped;
                return;
            }

            const Mesh& mesh = meshes[item.meshIndex];
            const size_t base = soup.positions.size() / 3;
            const size_t vertexCount = mesh.vertices.size();
            if (base + vertexCount > maxVertices)
            {
                overflowed = true;
                ++soup.drawItemsSkipped;
                return;
            }

            ++soup.drawItemsUsed;

            for (const auto& vertex : mesh.vertices)
            {
                float world[3];
                TransformPoint(item.model, vertex.position, world);
                for (int axis = 0; axis < 3; ++axis)
                {
                    soup.positions.push_back(world[axis]);
                    if (!soup.hasBounds)
                    {
                        soup.boundsMin[axis] = world[axis];
                        soup.boundsMax[axis] = world[axis];
                    }
                    else
                    {
                        soup.boundsMin[axis] = (std::min)(soup.boundsMin[axis], world[axis]);
                        soup.boundsMax[axis] = (std::max)(soup.boundsMax[axis], world[axis]);
                    }
                }
                soup.hasBounds = true;
            }

            const size_t triangleCount = TriangleCount(mesh);
            for (size_t tri = 0; tri < triangleCount; ++tri)
            {
                uint32_t local[3];
                bool outOfRange = false;
                for (size_t k = 0; k < 3; ++k)
                {
                    local[k] = mesh.HasIndices() ? mesh.indices[tri * 3 + k] : static_cast<uint32_t>(tri * 3 + k);
                    if (local[k] >= vertexCount)
                    {
                        outOfRange = true;
                    }
                }
                if (outOfRange)
                {
                    continue;
                }
                for (size_t k = 0; k < 3; ++k)
                {
                    soup.indices.push_back(static_cast<uint32_t>(base) + local[k]);
                }
            }
        }

        void ReserveForItem(const SceneSubmitter::DrawItem& item,
                            const std::vector<Mesh>& meshes,
                            size_t& reserveVertices,
                            size_t& reserveIndices)
        {
            if (!IsGatherable(item, meshes))
            {
                return;
            }
            const Mesh& mesh = meshes[item.meshIndex];
            reserveVertices += mesh.vertices.size();
            reserveIndices += TriangleCount(mesh) * 3;
        }
    }

    WorldTriangleSoup GatherOpaqueWorldTriangles(const std::vector<SceneSubmitter::DrawItem>& drawItems,
                                                 const std::vector<Mesh>& meshes)
    {
        WorldTriangleSoup soup;

        size_t reserveVertices = 0;
        size_t reserveIndices = 0;
        for (const SceneSubmitter::DrawItem& item : drawItems)
        {
            ReserveForItem(item, meshes, reserveVertices, reserveIndices);
        }
        soup.positions.reserve(reserveVertices * 3);
        soup.indices.reserve(reserveIndices);

        bool overflowed = false;
        for (const SceneSubmitter::DrawItem& item : drawItems)
        {
            AppendDrawItem(soup, item, meshes, overflowed);
        }

        return soup;
    }

    WorldTriangleSoup GatherOpaqueWorldTriangles(const std::vector<SceneSubmitter::DrawItem>& drawItems,
                                                 const std::vector<Mesh>& meshes,
                                                 const std::vector<uint32_t>& itemIndices)
    {
        WorldTriangleSoup soup;

        size_t reserveVertices = 0;
        size_t reserveIndices = 0;
        for (const uint32_t itemIndex : itemIndices)
        {
            if (itemIndex < drawItems.size())
            {
                ReserveForItem(drawItems[itemIndex], meshes, reserveVertices, reserveIndices);
            }
        }
        soup.positions.reserve(reserveVertices * 3);
        soup.indices.reserve(reserveIndices);

        bool overflowed = false;
        for (const uint32_t itemIndex : itemIndices)
        {
            if (itemIndex >= drawItems.size())
            {
                ++soup.drawItemsSkipped;
                continue;
            }
            AppendDrawItem(soup, drawItems[itemIndex], meshes, overflowed);
        }

        return soup;
    }
}
