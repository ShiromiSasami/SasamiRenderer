#pragma once

#include "PerfCase.h"
#include "Cases/VertexQuantizer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // Vertex quantisation (meshoptimizer "Vertex quantization" + octahedral normals, Cigolle et al.
    // 2014) on top of the meshopt vertex codec. Prepare welds the model, optimises it for vertex cache
    // and fetch, quantises the 48-byte vertices to 20 bytes (VertexQuantizer.h) and encodes both.
    // Baseline decodes the encoded 48-byte vertices, Optimized the encoded 20-byte ones; dequantising
    // is left to the vertex shader, so it is not timed. Only vertices are measured (indices are the
    // same in both variants, see MeshCodecCase). diskBytes = encoded vertex bytes, gpuBytes = decoded
    // vertex bytes. extras report the dequantisation error, which Check bounds.
    class MeshQuantizeCase : public IPerfCase
    {
    public:
        MeshQuantizeCase(std::string name, std::string assetPath);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }

        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        std::string m_name;
        std::string m_assetPath;
        std::string m_description;

        size_t m_vertexCount = 0;
        std::vector<Render::Vertex> m_vertices;
        std::vector<QuantizedVertex> m_quantized;
        VertexQuantizeBounds m_bounds;
        VertexQuantizeError m_error;
        std::vector<uint8_t> m_encoded[2];   // per variant
        std::vector<uint8_t> m_out[2];
        bool m_decodeFailed = false;
    };
}
