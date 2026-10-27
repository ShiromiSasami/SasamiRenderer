#include "Cases/MeshCodecCase.h"
#include "Cases/PerfMeshSource.h"

#include "Foundation/Tools/ScopedPerfTimer.h"
#include "meshoptimizer/meshoptimizer.h"

#include <cstdio>
#include <cstring>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        bool SameTriangle(const uint32_t* a, const uint32_t* b)
        {
            for (int r = 0; r < 3; ++r) {
                if (a[0] == b[r] && a[1] == b[(r + 1) % 3] && a[2] == b[(r + 2) % 3]) {
                    return true;
                }
            }
            return false;
        }
    }

    MeshCodecCase::MeshCodecCase(std::string name, std::string assetPath)
        : m_name(std::move(name))
        , m_assetPath(std::move(assetPath))
    {
        m_description = m_assetPath + ": raw copy vs meshopt vertex/index decode";
    }

    bool MeshCodecCase::Prepare(std::string& error)
    {
        PerfMeshSource source;
        if (!LoadWeldedMesh(m_assetPath, source, error)) {
            return false;
        }
        constexpr size_t kVertexSize = sizeof(Render::Vertex);
        static_assert(kVertexSize % 4 == 0 && kVertexSize <= 256, "meshopt vertex codec limits");
        m_vertexCount = source.vertices.size();
        m_indexCount = source.indices.size();
        m_indices = std::move(source.indices);
        const std::vector<Render::Vertex>& welded = source.vertices;

        meshopt_optimizeVertexCache(m_indices.data(), m_indices.data(), m_indexCount, m_vertexCount);
        m_vertices.resize(m_vertexCount * kVertexSize);
        meshopt_optimizeVertexFetch(m_vertices.data(), m_indices.data(), m_indexCount,
                                    welded.data(), m_vertexCount, kVertexSize);

        const auto start = Foundation::ScopedPerfTimer::Now();
        m_encodedVertices.resize(meshopt_encodeVertexBufferBound(m_vertexCount, kVertexSize));
        m_encodedVertices.resize(meshopt_encodeVertexBuffer(m_encodedVertices.data(), m_encodedVertices.size(),
                                                            m_vertices.data(), m_vertexCount, kVertexSize));
        m_encodedIndices.resize(meshopt_encodeIndexBufferBound(m_indexCount, m_vertexCount));
        m_encodedIndices.resize(meshopt_encodeIndexBuffer(m_encodedIndices.data(), m_encodedIndices.size(),
                                                          m_indices.data(), m_indexCount));
        m_encodeMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        if (m_encodedVertices.empty() || m_encodedIndices.empty()) {
            error = "meshopt encode failed";
            Release();
            return false;
        }

        for (uint32_t v = 0; v < 2; ++v) {
            m_outVertices[v].assign(m_vertices.size(), 0);
            m_outIndices[v].assign(m_indexCount, 0);
        }
        m_decodeFailed = false;

        const size_t rawBytes = m_vertices.size() + m_indexCount * sizeof(uint32_t);
        const size_t packedBytes = m_encodedVertices.size() + m_encodedIndices.size();
        char text[160];
        std::snprintf(text, sizeof(text), ": %zu verts %zu tris, raw %s -> %s (%.2fx), encode %.1f ms",
                      m_vertexCount, m_indexCount / 3, FormatKiB(rawBytes).c_str(), FormatKiB(packedBytes).c_str(),
                      static_cast<double>(rawBytes) / static_cast<double>(packedBytes), m_encodeMs);
        m_description = m_assetPath + text;
        return true;
    }

    PerfMetrics MeshCodecCase::RunVariant(PerfVariant variant)
    {
        const uint32_t v = static_cast<uint32_t>(variant);
        std::vector<uint8_t>& outVertices = m_outVertices[v];
        std::vector<uint32_t>& outIndices = m_outIndices[v];
        std::memset(outVertices.data(), 0, outVertices.size());
        std::memset(outIndices.data(), 0, outIndices.size() * sizeof(uint32_t));

        PerfMetrics metrics;
        const auto start = Foundation::ScopedPerfTimer::Now();
        if (variant == PerfVariant::Baseline) {
            std::memcpy(outVertices.data(), m_vertices.data(), m_vertices.size());
            std::memcpy(outIndices.data(), m_indices.data(), m_indexCount * sizeof(uint32_t));
        } else {
            const int vr = meshopt_decodeVertexBuffer(outVertices.data(), m_vertexCount, sizeof(Render::Vertex),
                                                      m_encodedVertices.data(), m_encodedVertices.size());
            const int ir = meshopt_decodeIndexBuffer(outIndices.data(), m_indexCount, sizeof(uint32_t),
                                                     m_encodedIndices.data(), m_encodedIndices.size());
            m_decodeFailed = m_decodeFailed || vr != 0 || ir != 0;
        }
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());

        const uint64_t rawBytes = m_vertices.size() + m_indexCount * sizeof(uint32_t);
        const uint64_t packedBytes = m_encodedVertices.size() + m_encodedIndices.size();
        // The decoded buffers are the same in both variants; only the image they come from differs.
        metrics.diskBytes = variant == PerfVariant::Baseline ? rawBytes : packedBytes;
        metrics.cpuBytes = metrics.diskBytes + rawBytes;
        return metrics;
    }

    bool MeshCodecCase::Check(std::string& detail)
    {
        if (m_decodeFailed) {
            detail = "meshopt decode returned an error";
            return false;
        }
        bool rotated = false;
        for (uint32_t v = 0; v < 2; ++v) {
            const char* name = PerfVariantName(static_cast<PerfVariant>(v));
            if (std::memcmp(m_outVertices[v].data(), m_vertices.data(), m_vertices.size()) != 0) {
                detail = std::string(name) + " vertices differ";
                return false;
            }
            if (v == 0) {
                if (std::memcmp(m_outIndices[v].data(), m_indices.data(), m_indexCount * sizeof(uint32_t)) != 0) {
                    detail = std::string(name) + " indices differ";
                    return false;
                }
                continue;
            }
            // The index codec keeps the triangle order but may rotate a triangle's vertices to match
            // its edge FIFO (indexcodec.cpp rotateTriangle); a rotation keeps the winding.
            for (size_t t = 0; t < m_indexCount; t += 3) {
                if (!SameTriangle(&m_outIndices[v][t], &m_indices[t])) {
                    detail = std::string(name) + " triangle " + std::to_string(t / 3) + " differs";
                    return false;
                }
            }
            rotated = std::memcmp(m_outIndices[v].data(), m_indices.data(), m_indexCount * sizeof(uint32_t)) != 0;
        }
        detail = std::string("vertices bit-identical, triangles equal") + (rotated ? " up to rotation" : "") + " ("
            + std::to_string(m_vertexCount) + " verts, " + std::to_string(m_indexCount / 3) + " tris)";
        return true;
    }

    void MeshCodecCase::Release()
    {
        m_vertices = {};
        m_indices = {};
        m_encodedVertices = {};
        m_encodedIndices = {};
        for (uint32_t v = 0; v < 2; ++v) {
            m_outVertices[v] = {};
            m_outIndices[v] = {};
        }
    }
}
