#include "Cases/MeshQuantizeCase.h"
#include "Cases/PerfMeshSource.h"

#include "Foundation/Tools/ScopedPerfTimer.h"
#include "meshoptimizer/meshoptimizer.h"

#include <cstdio>
#include <cstring>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        constexpr size_t kStride[2] = { sizeof(Render::Vertex), sizeof(QuantizedVertex) };
        static_assert(sizeof(Render::Vertex) % 4 == 0 && sizeof(Render::Vertex) <= 256, "meshopt vertex codec limits");
        static_assert(sizeof(QuantizedVertex) % 4 == 0, "meshopt vertex codec limits");

        // 2^-11: half keeps 11 significant bits, so round-to-nearest is within half of that.
        constexpr double kMaxUvRelErr = 1.0 / 2048.0;
        // 16-bit octahedral: about 0.005 deg typical; the bound is loose on purpose.
        constexpr double kMaxNormalErrDeg = 0.05;

        bool Encode(const void* vertices, size_t count, size_t stride, std::vector<uint8_t>& out)
        {
            out.resize(meshopt_encodeVertexBufferBound(count, stride));
            out.resize(meshopt_encodeVertexBuffer(out.data(), out.size(), vertices, count, stride));
            return !out.empty();
        }
    }

    MeshQuantizeCase::MeshQuantizeCase(std::string name, std::string assetPath)
        : m_name(std::move(name))
        , m_assetPath(std::move(assetPath))
    {
        m_description = m_assetPath + ": decode 48 B float vertices vs 20 B quantised vertices";
    }

    bool MeshQuantizeCase::Prepare(std::string& error)
    {
        PerfMeshSource source;
        if (!LoadWeldedMesh(m_assetPath, source, error)) {
            return false;
        }
        m_vertexCount = source.vertices.size();
        std::vector<uint32_t>& indices = source.indices;
        meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), m_vertexCount);
        m_vertices.resize(m_vertexCount);
        meshopt_optimizeVertexFetch(m_vertices.data(), indices.data(), indices.size(),
                                    source.vertices.data(), m_vertexCount, sizeof(Render::Vertex));

        const auto start = Foundation::ScopedPerfTimer::Now();
        m_bounds = ComputeQuantizeBounds(m_vertices);
        QuantizeVertices(m_vertices, m_bounds, m_quantized);
        const double quantizeMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        m_error = MeasureQuantizeError(m_vertices, m_quantized, m_bounds);

        if (!Encode(m_vertices.data(), m_vertexCount, kStride[0], m_encoded[0])
            || !Encode(m_quantized.data(), m_vertexCount, kStride[1], m_encoded[1])) {
            error = "meshopt encode failed";
            Release();
            return false;
        }
        for (uint32_t v = 0; v < 2; ++v) {
            m_out[v].assign(m_vertexCount * kStride[v], 0);
        }
        m_decodeFailed = false;

        char text[200];
        std::snprintf(text, sizeof(text), ": %zu verts, encoded %s -> %s, raw %s -> %s, quantize %.1f ms",
                      m_vertexCount, FormatKiB(m_encoded[0].size()).c_str(), FormatKiB(m_encoded[1].size()).c_str(),
                      FormatKiB(m_vertexCount * kStride[0]).c_str(), FormatKiB(m_vertexCount * kStride[1]).c_str(),
                      quantizeMs);
        m_description = m_assetPath + text;
        return true;
    }

    PerfMetrics MeshQuantizeCase::RunVariant(PerfVariant variant)
    {
        const uint32_t v = static_cast<uint32_t>(variant);
        std::vector<uint8_t>& out = m_out[v];
        std::memset(out.data(), 0, out.size());

        PerfMetrics metrics;
        const auto start = Foundation::ScopedPerfTimer::Now();
        const int result = meshopt_decodeVertexBuffer(out.data(), m_vertexCount, kStride[v],
                                                      m_encoded[v].data(), m_encoded[v].size());
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        m_decodeFailed = m_decodeFailed || result != 0;

        metrics.diskBytes = m_encoded[v].size();
        metrics.gpuBytes = out.size();
        metrics.cpuBytes = metrics.diskBytes + out.size();
        metrics.extras = {
            { "vertexStride", static_cast<double>(kStride[v]) },
            { "posStepM", m_error.positionStep },
            { "maxPosErrM", v == 0 ? 0.0 : m_error.maxPositionErr },
            { "maxNormalErrDeg", v == 0 ? 0.0 : m_error.maxNormalErrDeg },
            { "maxUvRelErr", v == 0 ? 0.0 : m_error.maxUvRelErr },
            { "maxColorErr", v == 0 ? 0.0 : m_error.maxColorErr },
        };
        return metrics;
    }

    bool MeshQuantizeCase::Check(std::string& detail)
    {
        if (m_decodeFailed) {
            detail = "meshopt decode returned an error";
            return false;
        }
        if (std::memcmp(m_out[0].data(), m_vertices.data(), m_out[0].size()) != 0) {
            detail = "Baseline vertices differ";
            return false;
        }
        if (std::memcmp(m_out[1].data(), m_quantized.data(), m_out[1].size()) != 0) {
            detail = "Optimized vertices differ from the quantised source";
            return false;
        }
        char text[400];
        std::snprintf(text, sizeof(text),
                      "decode bit-identical; posErr %.3g (step %.3g slack %.3g) normalErr %.4f deg uvRelErr %.3g (uv %.7g -> %.7g) "
                      "colorErr %.4f (clamped %u, zeroNormals %u, uvOverflow %u, uvFlushed %u)",
                      m_error.maxPositionErr, m_error.positionStep, m_error.positionSlack, m_error.maxNormalErrDeg, m_error.maxUvRelErr,
                      m_error.worstUvSource, m_error.worstUvDecoded, m_error.maxColorErr, m_error.colorClamped, m_error.zeroNormals,
                      m_error.uvOverflow, m_error.uvFlushed);
        detail = text;
        // unorm rounding is within half a step; the float maths around it adds a few ulp
        // (Sponza, coords up to ~1900: 0.0287 vs half step 0.0284).
        if (m_error.maxPositionErr > m_error.positionStep * 0.5 + m_error.positionSlack) {
            detail = "position error above half a step: " + detail;
            return false;
        }
        if (m_error.maxNormalErrDeg > kMaxNormalErrDeg) {
            detail = "normal error too large: " + detail;
            return false;
        }
        if (m_error.maxUvRelErr > kMaxUvRelErr) {
            detail = "uv error too large: " + detail;
            return false;
        }
        // Colour is reported only: sources may store values outside [0, 1], which unorm8 clamps.
        return true;
    }

    void MeshQuantizeCase::Release()
    {
        m_vertices = {};
        m_quantized = {};
        for (uint32_t v = 0; v < 2; ++v) {
            m_encoded[v] = {};
            m_out[v] = {};
        }
    }
}
