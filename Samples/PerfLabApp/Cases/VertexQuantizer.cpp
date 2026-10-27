#include "Cases/VertexQuantizer.h"

#include "meshoptimizer/meshoptimizer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        float SignNotZero(float v)
        {
            return v >= 0.0f ? 1.0f : -1.0f;
        }

        // Unit vector -> octahedron in [-1, 1]^2 (Cigolle et al. 2014, listing 1).
        void EncodeOct(const float n[3], float& u, float& v)
        {
            const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
            u = n[0] / l1;
            v = n[1] / l1;
            if (n[2] < 0.0f) {
                const float pu = u;
                u = (1.0f - std::fabs(v)) * SignNotZero(pu);
                v = (1.0f - std::fabs(pu)) * SignNotZero(v);
            }
        }

        void DecodeOct(float u, float v, float n[3])
        {
            n[0] = u;
            n[1] = v;
            n[2] = 1.0f - std::fabs(u) - std::fabs(v);
            if (n[2] < 0.0f) {
                const float px = n[0];
                n[0] = (1.0f - std::fabs(n[1])) * SignNotZero(px);
                n[1] = (1.0f - std::fabs(px)) * SignNotZero(n[1]);
            }
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        }

        // Normalised source normal; false (and +z) for a zero-length one.
        bool UnitNormal(const Render::Vertex& vertex, float n[3])
        {
            const float len = std::sqrt(vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1]
                                        + vertex.normal[2] * vertex.normal[2]);
            if (!(len > 1e-20f)) {
                n[0] = 0.0f;
                n[1] = 0.0f;
                n[2] = 1.0f;
                return false;
            }
            n[0] = vertex.normal[0] / len;
            n[1] = vertex.normal[1] / len;
            n[2] = vertex.normal[2] / len;
            return true;
        }

        constexpr float kSnorm16 = 32767.0f;
    }

    VertexQuantizeBounds ComputeQuantizeBounds(const std::vector<Render::Vertex>& vertices)
    {
        VertexQuantizeBounds bounds;
        if (vertices.empty()) {
            return bounds;
        }
        float maxv[3];
        for (int a = 0; a < 3; ++a) {
            bounds.min[a] = vertices[0].position[a];
            maxv[a] = vertices[0].position[a];
        }
        for (const Render::Vertex& vertex : vertices) {
            for (int a = 0; a < 3; ++a) {
                bounds.min[a] = (std::min)(bounds.min[a], vertex.position[a]);
                maxv[a] = (std::max)(maxv[a], vertex.position[a]);
            }
        }
        for (int a = 0; a < 3; ++a) {
            bounds.extent[a] = maxv[a] - bounds.min[a];
        }
        return bounds;
    }

    void QuantizeVertices(const std::vector<Render::Vertex>& vertices, const VertexQuantizeBounds& bounds,
                          std::vector<QuantizedVertex>& out)
    {
        out.resize(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            const Render::Vertex& src = vertices[i];
            QuantizedVertex& q = out[i];
            for (int a = 0; a < 3; ++a) {
                const float t = bounds.extent[a] > 0.0f ? (src.position[a] - bounds.min[a]) / bounds.extent[a] : 0.0f;
                q.position[a] = static_cast<uint16_t>(meshopt_quantizeUnorm(t, 16));
            }
            q.position[3] = 0;

            float n[3];
            UnitNormal(src, n);
            float u = 0.0f;
            float v = 0.0f;
            EncodeOct(n, u, v);
            q.normalOct[0] = static_cast<int16_t>(meshopt_quantizeSnorm(u, 16));
            q.normalOct[1] = static_cast<int16_t>(meshopt_quantizeSnorm(v, 16));

            for (int c = 0; c < 4; ++c) {
                q.color[c] = static_cast<uint8_t>(meshopt_quantizeUnorm(src.color[c], 8));
            }
            q.uv[0] = meshopt_quantizeHalf(src.uv[0]);
            q.uv[1] = meshopt_quantizeHalf(src.uv[1]);
        }
    }

    Render::Vertex DequantizeVertex(const QuantizedVertex& q, const VertexQuantizeBounds& bounds)
    {
        Render::Vertex out{};
        for (int a = 0; a < 3; ++a) {
            out.position[a] = bounds.min[a] + static_cast<float>(q.position[a]) / 65535.0f * bounds.extent[a];
        }
        DecodeOct(static_cast<float>(q.normalOct[0]) / kSnorm16, static_cast<float>(q.normalOct[1]) / kSnorm16,
                  out.normal);
        for (int c = 0; c < 4; ++c) {
            out.color[c] = static_cast<float>(q.color[c]) / 255.0f;
        }
        out.uv[0] = meshopt_dequantizeHalf(q.uv[0]);
        out.uv[1] = meshopt_dequantizeHalf(q.uv[1]);
        return out;
    }

    VertexQuantizeError MeasureQuantizeError(const std::vector<Render::Vertex>& source,
                                             const std::vector<QuantizedVertex>& quantized,
                                             const VertexQuantizeBounds& bounds)
    {
        VertexQuantizeError err;
        for (int a = 0; a < 3; ++a) {
            err.positionStep = (std::max)(err.positionStep, static_cast<double>(bounds.extent[a]) / 65535.0);
            const double maxAbs = (std::max)(std::fabs(static_cast<double>(bounds.min[a])),
                                             std::fabs(static_cast<double>(bounds.min[a]) + bounds.extent[a]));
            err.positionSlack = (std::max)(err.positionSlack, 4.0 * FLT_EPSILON * maxAbs);
        }
        const size_t count = (std::min)(source.size(), quantized.size());
        for (size_t i = 0; i < count; ++i) {
            const Render::Vertex& src = source[i];
            const Render::Vertex dq = DequantizeVertex(quantized[i], bounds);
            for (int a = 0; a < 3; ++a) {
                err.maxPositionErr = (std::max)(err.maxPositionErr,
                    std::fabs(static_cast<double>(src.position[a]) - static_cast<double>(dq.position[a])));
            }

            float n[3];
            if (!UnitNormal(src, n)) {
                ++err.zeroNormals;
            }
            const double dot = static_cast<double>(n[0]) * dq.normal[0] + static_cast<double>(n[1]) * dq.normal[1]
                + static_cast<double>(n[2]) * dq.normal[2];
            const double angle = std::acos((std::min)(1.0, (std::max)(-1.0, dot))) * 180.0 / kPi;
            err.maxNormalErrDeg = (std::max)(err.maxNormalErrDeg, angle);

            for (int c = 0; c < 4; ++c) {
                if (src.color[c] < 0.0f || src.color[c] > 1.0f) {
                    ++err.colorClamped;
                }
                err.maxColorErr = (std::max)(err.maxColorErr,
                    std::fabs(static_cast<double>(src.color[c]) - static_cast<double>(dq.color[c])));
            }
            for (int c = 0; c < 2; ++c) {
                const double s = src.uv[c];
                if (std::fabs(s) > 65504.0) {
                    ++err.uvOverflow;
                    continue;
                }
                if (std::fabs(s) < 6.103515625e-5) {
                    if (s != 0.0) {
                        ++err.uvFlushed;
                    }
                    continue;
                }
                const double rel = std::fabs(s - static_cast<double>(dq.uv[c])) / std::fabs(s);
                if (rel > err.maxUvRelErr) {
                    err.maxUvRelErr = rel;
                    err.worstUvSource = s;
                    err.worstUvDecoded = dq.uv[c];
                }
            }
        }
        return err;
    }
}
