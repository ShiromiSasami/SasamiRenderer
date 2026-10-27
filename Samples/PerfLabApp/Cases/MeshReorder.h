#pragma once

#include "PerfCase.h"
#include "Renderer/Structures/Vertex.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // Which meshopt steps ReorderForVertexCache runs (stage 2e splits Full to find what costs time).
    enum class MeshReorderMode
    {
        Full,         // vertex cache -> overdraw -> vertex fetch remap
        NoOverdraw,   // vertex cache -> vertex fetch remap
        CacheOnly,    // vertex cache only; the vertex buffer is copied unchanged
        OverdrawOnly, // vertex cache -> overdraw, no fetch remap (stage 2f: Full without the overfetch change)
    };

    const char* MeshReorderModeName(MeshReorderMode mode);

    // meshopt vertex cache -> overdraw (threshold 1.05, meshoptimizer README) -> vertex fetch remap.
    // outVertices holds only the vertices the index buffer uses (CacheOnly: all of them).
    void ReorderForVertexCache(const std::vector<Render::Vertex>& vertices, const std::vector<uint32_t>& indices,
                               std::vector<Render::Vertex>& outVertices, std::vector<uint32_t>& outIndices,
                               MeshReorderMode mode = MeshReorderMode::Full);

    // meshopt analyze*: acmr/atvr (FIFO 16), overfetch and overdraw, as PerfMetrics extras.
    std::vector<PerfExtraMetric> AnalyzeMeshOrder(const std::vector<Render::Vertex>& vertices,
                                                  const std::vector<uint32_t>& indices);

    struct MeshOrderComparison
    {
        bool sizesOk = false;
        bool sameTriangles = false;
        size_t unmatched = 0;   // optimised vertices with no byte-identical base vertex
    };

    // Does the reordered mesh hold the same triangles (up to rotation within a triangle, winding
    // kept) as the base mesh? Vertices are matched by bytes, so the base must be welded (unique).
    MeshOrderComparison CompareMeshOrder(const std::vector<Render::Vertex>& baseVertices,
                                         const std::vector<uint32_t>& baseIndices,
                                         const std::vector<Render::Vertex>& optVertices,
                                         const std::vector<uint32_t>& optIndices);
}
