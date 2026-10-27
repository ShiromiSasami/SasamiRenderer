#pragma once

#include "PerfCase.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // Vertex/index buffer compression with meshoptimizer's codecs (meshopt_encodeVertexBuffer /
    // meshopt_encodeIndexBuffer). Prepare loads a model, merges its meshes, removes duplicate
    // vertices and optimises for vertex cache and fetch (the encoder needs that to work well), then
    // encodes once. Baseline copies the raw 48-byte vertices and 32-bit indices from an in-memory
    // image; Optimized decodes the compressed image. diskBytes is the size of the image each variant
    // reads, so the case shows what the smaller file costs in decode time. Check: vertices are
    // bit-identical; indices keep the triangle order, but the index codec may rotate a triangle's
    // three vertices (same winding), so Optimized compares triangles up to rotation.
    class MeshCodecCase : public IPerfCase
    {
    public:
        MeshCodecCase(std::string name, std::string assetPath);

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
        size_t m_indexCount = 0;
        std::vector<uint8_t> m_vertices;      // optimised source, vertexCount * 48 bytes
        std::vector<uint32_t> m_indices;
        std::vector<uint8_t> m_encodedVertices;
        std::vector<uint8_t> m_encodedIndices;
        double m_encodeMs = 0.0;

        std::vector<uint8_t> m_outVertices[2];
        std::vector<uint32_t> m_outIndices[2];
        bool m_decodeFailed = false;
    };
}
