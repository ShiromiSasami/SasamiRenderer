#include "Cases/MeshReorder.h"

#include "meshoptimizer/meshoptimizer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        constexpr unsigned kCacheSize = 16;          // meshopt's default FIFO size, also used inside optimizeOverdraw
        constexpr float kOverdrawThreshold = 1.05f;  // allow up to 5% worse ACMR for less overdraw (meshoptimizer README)
        constexpr size_t kVertexSize = sizeof(Render::Vertex);

        using Triangle = std::array<uint32_t, 3>;

        // Triangles as a sorted multiset; each one rotated so its smallest index comes first.
        std::vector<Triangle> CanonicalTriangles(const std::vector<uint32_t>& indices)
        {
            std::vector<Triangle> tris(indices.size() / 3);
            for (size_t t = 0; t < tris.size(); ++t) {
                Triangle tri = { indices[t * 3 + 0], indices[t * 3 + 1], indices[t * 3 + 2] };
                const auto smallest = std::min_element(tri.begin(), tri.end());
                std::rotate(tri.begin(), smallest, tri.end());
                tris[t] = tri;
            }
            std::sort(tris.begin(), tris.end());
            return tris;
        }

        bool SameVertex(const Render::Vertex& a, const Render::Vertex& b)
        {
            return std::memcmp(&a, &b, kVertexSize) == 0;
        }
    }

    const char* MeshReorderModeName(MeshReorderMode mode)
    {
        switch (mode) {
        case MeshReorderMode::Full: return "full";
        case MeshReorderMode::NoOverdraw: return "nooverdraw";
        case MeshReorderMode::CacheOnly: return "cacheonly";
        case MeshReorderMode::OverdrawOnly: return "overdrawonly";
        }
        return "?";
    }

    void ReorderForVertexCache(const std::vector<Render::Vertex>& vertices, const std::vector<uint32_t>& indices,
                               std::vector<Render::Vertex>& outVertices, std::vector<uint32_t>& outIndices,
                               MeshReorderMode mode)
    {
        const size_t indexCount = indices.size();
        const size_t vertexCount = vertices.size();
        std::vector<uint32_t> reordered(indexCount);
        meshopt_optimizeVertexCache(reordered.data(), indices.data(), indexCount, vertexCount);
        if (mode == MeshReorderMode::Full || mode == MeshReorderMode::OverdrawOnly) {
            // In place is allowed (meshopt copies the input when dst == indices).
            meshopt_optimizeOverdraw(reordered.data(), reordered.data(), indexCount, vertices[0].position,
                                     vertexCount, kVertexSize, kOverdrawThreshold);
        }
        if (mode == MeshReorderMode::CacheOnly || mode == MeshReorderMode::OverdrawOnly) {
            outIndices = std::move(reordered);
            outVertices = vertices;
            return;
        }
        std::vector<uint32_t> remap(vertexCount);
        const size_t used = meshopt_optimizeVertexFetchRemap(remap.data(), reordered.data(), indexCount, vertexCount);
        outIndices.resize(indexCount);
        meshopt_remapIndexBuffer(outIndices.data(), reordered.data(), indexCount, remap.data());
        outVertices.resize(used);
        meshopt_remapVertexBuffer(outVertices.data(), vertices.data(), vertexCount, kVertexSize, remap.data());
    }

    std::vector<PerfExtraMetric> AnalyzeMeshOrder(const std::vector<Render::Vertex>& vertices,
                                                  const std::vector<uint32_t>& indices)
    {
        const meshopt_VertexCacheStatistics cache = meshopt_analyzeVertexCache(
            indices.data(), indices.size(), vertices.size(), kCacheSize, 0, 0);
        const meshopt_VertexFetchStatistics fetch = meshopt_analyzeVertexFetch(
            indices.data(), indices.size(), vertices.size(), kVertexSize);
        const meshopt_OverdrawStatistics overdraw = meshopt_analyzeOverdraw(
            indices.data(), indices.size(), vertices[0].position, vertices.size(), kVertexSize);
        return {
            { "acmr", cache.acmr },
            { "atvr", cache.atvr },
            { "overfetch", fetch.overfetch },
            { "overdraw", overdraw.overdraw },
        };
    }

    MeshOrderComparison CompareMeshOrder(const std::vector<Render::Vertex>& baseVertices,
                                         const std::vector<uint32_t>& baseIndices,
                                         const std::vector<Render::Vertex>& optVertices,
                                         const std::vector<uint32_t>& optIndices)
    {
        MeshOrderComparison result;
        if (baseIndices.size() != optIndices.size() || optVertices.size() > baseVertices.size() || optVertices.empty()) {
            return result;
        }
        result.sizesOk = true;

        // Map each optimised vertex back to its welded source. The remap is a bijection between
        // used vertices, so sort the base by bytes and look each optimised vertex up.
        auto less = [](const Render::Vertex* a, const Render::Vertex* b) {
            return std::memcmp(a, b, kVertexSize) < 0;
        };
        std::vector<const Render::Vertex*> baseSorted(baseVertices.size());
        for (size_t i = 0; i < baseVertices.size(); ++i) {
            baseSorted[i] = &baseVertices[i];
        }
        std::sort(baseSorted.begin(), baseSorted.end(), less);
        std::vector<uint32_t> source(optVertices.size(), UINT32_MAX);
        for (size_t i = 0; i < optVertices.size(); ++i) {
            const auto it = std::lower_bound(baseSorted.begin(), baseSorted.end(), &optVertices[i], less);
            if (it == baseSorted.end() || !SameVertex(**it, optVertices[i])) {
                ++result.unmatched;
                continue;
            }
            source[i] = static_cast<uint32_t>(*it - baseVertices.data());
        }
        std::vector<uint32_t> mapped(optIndices.size());
        for (size_t i = 0; i < optIndices.size(); ++i) {
            mapped[i] = optIndices[i] < source.size() ? source[optIndices[i]] : UINT32_MAX;
        }
        result.sameTriangles = result.unmatched == 0 && CanonicalTriangles(mapped) == CanonicalTriangles(baseIndices);
        return result;
    }
}
