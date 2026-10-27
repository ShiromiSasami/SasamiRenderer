#include "Cases/VertexCacheCase.h"
#include "Cases/MeshReorder.h"
#include "Cases/PerfMeshSource.h"

#include "Foundation/Tools/ScopedPerfTimer.h"

#include <cstdio>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        constexpr size_t kVertexSize = sizeof(Render::Vertex);
    }

    VertexCacheCase::VertexCacheCase(std::string name, std::string assetPath)
        : m_name(std::move(name))
        , m_assetPath(std::move(assetPath))
    {
        m_description = m_assetPath + ": loader order vs meshopt vertex cache + overdraw + fetch reorder"
            " (cpuMs = reorder cost at load; see extras)";
    }

    bool VertexCacheCase::Prepare(std::string& error)
    {
        PerfMeshSource source;
        if (!LoadWeldedMesh(m_assetPath, source, error)) {
            return false;
        }
        m_vertices = std::move(source.vertices);
        m_indices = std::move(source.indices);
        for (uint32_t v = 0; v < 2; ++v) {
            m_analyzed[v] = false;
            m_extras[v].clear();
        }
        return true;
    }

    PerfMetrics VertexCacheCase::RunVariant(PerfVariant variant)
    {
        const uint32_t v = static_cast<uint32_t>(variant);
        std::vector<Render::Vertex>& outVertices = m_outVertices[v];
        std::vector<uint32_t>& outIndices = m_outIndices[v];
        const auto start = Foundation::ScopedPerfTimer::Now();
        if (variant == PerfVariant::Baseline) {
            outIndices = m_indices;
            outVertices = m_vertices;
        } else {
            ReorderForVertexCache(m_vertices, m_indices, outVertices, outIndices);
        }
        PerfMetrics metrics;
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());

        // The analysers are not part of the timed section, and their result does not change between runs.
        if (!m_analyzed[v]) {
            m_extras[v] = AnalyzeMeshOrder(outVertices, outIndices);
            m_analyzed[v] = true;
        }
        metrics.extras = m_extras[v];
        metrics.cpuBytes = outVertices.size() * kVertexSize + outIndices.size() * sizeof(uint32_t);
        metrics.gpuBytes = metrics.cpuBytes;
        return metrics;
    }

    bool VertexCacheCase::Check(std::string& detail)
    {
        const std::vector<Render::Vertex>& baseV = m_outVertices[0];
        const std::vector<uint32_t>& baseI = m_outIndices[0];
        const std::vector<Render::Vertex>& optV = m_outVertices[1];
        const std::vector<uint32_t>& optI = m_outIndices[1];
        const MeshOrderComparison cmp = CompareMeshOrder(baseV, baseI, optV, optI);
        char text[256] = {};
        if (!cmp.sizesOk) {
            std::snprintf(text, sizeof(text), "size mismatch: indices %zu/%zu vertices %zu/%zu",
                          baseI.size(), optI.size(), baseV.size(), optV.size());
            detail = text;
            return false;
        }

        const double base0 = m_extras[0].empty() ? 0.0 : m_extras[0][0].value;
        const double opt0 = m_extras[1].empty() ? 0.0 : m_extras[1][0].value;
        std::snprintf(text, sizeof(text), "tris=%zu verts=%zu usedVerts=%zu unmatched=%zu triangles=%s acmr %.3f->%.3f",
                      baseI.size() / 3, baseV.size(), optV.size(), cmp.unmatched, cmp.sameTriangles ? "same" : "DIFFER",
                      base0, opt0);
        detail = text;
        return cmp.sameTriangles;
    }

    void VertexCacheCase::Release()
    {
        m_vertices = {};
        m_indices = {};
        for (uint32_t v = 0; v < 2; ++v) {
            m_outVertices[v] = {};
            m_outIndices[v] = {};
        }
    }
}
