#pragma once

#include "PerfCase.h"
#include "Renderer/Structures/Vertex.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // Index/vertex reordering for the post-transform cache, overdraw and vertex fetch
    // (meshopt_optimizeVertexCache -> meshopt_optimizeOverdraw(1.05) -> meshopt_optimizeVertexFetchRemap).
    // The renderer draws meshes in loader order, so Baseline is that order (an index copy) and
    // Optimized is the full reorder. cpuMs is therefore the load-time cost of the reorder; Baseline
    // is near 0 and a speedup below 1 is expected. The benefit is in the extras, computed once per
    // variant with meshoptimizer's analyzers (simplified FIFO cache of 16, software rasteriser; the
    // header says they "may not match actual GPU performance"): acmr/atvr (vertex shader invocations
    // per triangle / per vertex), overfetch (bytes fetched / vertex buffer size) and overdraw.
    // GPU draw time is not measured here (stage 2b). Check: both variants hold the same set of
    // triangles (each rotated to start at its smallest vertex, winding kept).
    class VertexCacheCase : public IPerfCase
    {
    public:
        VertexCacheCase(std::string name, std::string assetPath);

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

        std::vector<Render::Vertex> m_vertices;   // welded, loader order
        std::vector<uint32_t> m_indices;

        std::vector<Render::Vertex> m_outVertices[2];
        std::vector<uint32_t> m_outIndices[2];
        std::vector<PerfExtraMetric> m_extras[2];
        bool m_analyzed[2] = {};
    };
}
