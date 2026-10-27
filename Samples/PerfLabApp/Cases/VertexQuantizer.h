#pragma once

#include "Renderer/Structures/Vertex.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // 20-byte vertex: position 16-bit unorm in the mesh AABB, normal octahedral 2x16-bit snorm
    // (Cigolle et al. 2014, "A Survey of Efficient Representations for Independent Unit Vectors"),
    // colour 8-bit unorm, uv IEEE half. A shader would rebuild position as min + q/65535 * extent.
    struct QuantizedVertex
    {
        uint16_t position[4];   // x, y, z, w = 0 (keeps the stride a multiple of 4)
        int16_t normalOct[2];
        uint8_t color[4];
        uint16_t uv[2];
    };
    static_assert(sizeof(QuantizedVertex) == 20, "QuantizedVertex layout");

    struct VertexQuantizeBounds
    {
        float min[3] = {};
        float extent[3] = {};   // max - min, 0 on a flat axis
    };

    // Largest dequantisation error over all vertices.
    struct VertexQuantizeError
    {
        double maxPositionErr = 0.0;   // per axis, model units
        double positionStep = 0.0;     // largest extent / 65535
        double positionSlack = 0.0;    // float rounding of (p-min)/extent and back: 4 ulp of the largest |coord|
        double maxNormalErrDeg = 0.0;  // angle between source (normalised) and decoded normal
        double maxUvRelErr = 0.0;      // |uv - uv'| / |uv| over |uv| >= 2^-14 (the smallest normal half)
        double worstUvSource = 0.0;    // the uv component that gave maxUvRelErr
        double worstUvDecoded = 0.0;
        double maxColorErr = 0.0;
        uint32_t colorClamped = 0;     // colour components outside [0, 1]
        uint32_t uvOverflow = 0;       // |uv| above the half range (65504)
        uint32_t uvFlushed = 0;        // 0 < |uv| < 2^-14: meshopt_quantizeHalf flushes these to 0 (abs error < 6.1e-5)
        uint32_t zeroNormals = 0;      // source normals of length 0, stored as +z
    };

    VertexQuantizeBounds ComputeQuantizeBounds(const std::vector<Render::Vertex>& vertices);
    void QuantizeVertices(const std::vector<Render::Vertex>& vertices, const VertexQuantizeBounds& bounds,
                          std::vector<QuantizedVertex>& out);
    Render::Vertex DequantizeVertex(const QuantizedVertex& q, const VertexQuantizeBounds& bounds);
    VertexQuantizeError MeasureQuantizeError(const std::vector<Render::Vertex>& source,
                                             const std::vector<QuantizedVertex>& quantized,
                                             const VertexQuantizeBounds& bounds);
}
