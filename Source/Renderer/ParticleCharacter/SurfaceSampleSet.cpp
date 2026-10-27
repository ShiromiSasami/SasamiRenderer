#include "Renderer/ParticleCharacter/SurfaceSampleSet.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::ParticleCharacter
{
    namespace
    {
        // PCG32 XSH RR (O'Neill 2014), seeded with pcg32_srandom_r(initstate, initseq).
        class Pcg32
        {
        public:
            Pcg32(uint64_t initState, uint64_t initSeq)
            {
                m_state = 0;
                m_inc = (initSeq << 1) | 1ULL;
                Next();
                m_state += initState;
                Next();
            }

            uint32_t Next()
            {
                const uint64_t old = m_state;
                m_state = old * 6364136223846793005ULL + m_inc;
                const uint32_t xorShifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
                const uint32_t rot = static_cast<uint32_t>(old >> 59u);
                return (xorShifted >> rot) | (xorShifted << ((~rot + 1u) & 31u));
            }

            float NextFloat() { return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f); }

        private:
            uint64_t m_state = 0;
            uint64_t m_inc = 1;
        };

        struct SurfaceTriangle
        {
            uint32_t vertex[3];
            uint32_t mesh;
        };
    }

    bool SampleSkinnedSurface(const std::vector<SkinnedMesh>& meshes, uint32_t count, uint32_t seed,
                              SurfaceSampleSet& out, std::string& error)
    {
        out = {};
        if (count == 0)
        {
            error = "sample count is 0";
            return false;
        }

        std::vector<uint32_t> meshOffsets(meshes.size());
        uint32_t vertexTotal = 0;
        for (size_t m = 0; m < meshes.size(); ++m)
        {
            meshOffsets[m] = vertexTotal;
            vertexTotal += meshes[m].VertexCount();
        }
        if (vertexTotal == 0)
        {
            error = "meshes have no vertices";
            return false;
        }

        std::vector<SurfaceTriangle> triangles;
        std::vector<double> cdf;
        double totalArea = 0.0;
        for (size_t m = 0; m < meshes.size(); ++m)
        {
            const SkinnedMesh& mesh = meshes[m];
            const uint32_t meshVertexCount = mesh.VertexCount();
            const uint32_t indexCount = mesh.HasIndices() ? mesh.IndexCount() : meshVertexCount;
            for (uint32_t k = 0; k + 3 <= indexCount; k += 3)
            {
                uint32_t local[3];
                for (uint32_t c = 0; c < 3; ++c)
                {
                    local[c] = mesh.HasIndices() ? mesh.indices[k + c] : k + c;
                }
                if (local[0] >= meshVertexCount || local[1] >= meshVertexCount || local[2] >= meshVertexCount)
                {
                    continue;
                }

                const float* p0 = mesh.vertices[local[0]].position;
                const float* p1 = mesh.vertices[local[1]].position;
                const float* p2 = mesh.vertices[local[2]].position;
                const double e1[3] = { double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2] };
                const double e2[3] = { double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2] };
                const double cx = e1[1] * e2[2] - e1[2] * e2[1];
                const double cy = e1[2] * e2[0] - e1[0] * e2[2];
                const double cz = e1[0] * e2[1] - e1[1] * e2[0];
                const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
                if (!(area > 0.0))
                {
                    continue;
                }

                totalArea += area;
                cdf.push_back(totalArea);
                SurfaceTriangle tri;
                for (uint32_t c = 0; c < 3; ++c)
                {
                    tri.vertex[c] = meshOffsets[m] + local[c];
                }
                tri.mesh = static_cast<uint32_t>(m);
                triangles.push_back(tri);
            }
        }
        if (triangles.empty() || !(totalArea > 0.0))
        {
            error = "meshes have zero surface area";
            return false;
        }

        // Global vertex index -> (mesh, local) for the uv lookup.
        auto vertexUv = [&](uint32_t mesh, uint32_t global) -> const float*
        {
            return meshes[mesh].vertices[global - meshOffsets[mesh]].uv;
        };

        Pcg32 rng(seed, 0x5A5A1234u);
        out.samples.resize(count);
        for (uint32_t s = 0; s < count; ++s)
        {
            const float r0 = rng.NextFloat();
            const float r1 = rng.NextFloat();
            const float r2 = rng.NextFloat();

            const double target = static_cast<double>(r0) * totalArea;
            size_t ti = static_cast<size_t>(std::upper_bound(cdf.begin(), cdf.end(), target) - cdf.begin());
            ti = (std::min)(ti, triangles.size() - 1);
            const SurfaceTriangle& tri = triangles[ti];

            const float u = std::sqrt(r1);
            const float b0 = 1.0f - u;
            const float b1 = u * (1.0f - r2);
            const float b2 = u * r2;

            SurfaceSample& sample = out.samples[s];
            for (uint32_t c = 0; c < 3; ++c)
            {
                sample.vertex[c] = tri.vertex[c];
            }
            sample.b1 = b1;
            sample.b2 = b2;
            const float* uv0 = vertexUv(tri.mesh, tri.vertex[0]);
            const float* uv1 = vertexUv(tri.mesh, tri.vertex[1]);
            const float* uv2 = vertexUv(tri.mesh, tri.vertex[2]);
            sample.uv[0] = b0 * uv0[0] + b1 * uv1[0] + b2 * uv2[0];
            sample.uv[1] = b0 * uv0[1] + b1 * uv1[1] + b2 * uv2[1];
            sample.mesh = tri.mesh;
        }

        out.vertexCount = vertexTotal;
        out.totalArea = totalArea;
        out.triangleCount = static_cast<uint32_t>(triangles.size());
        return true;
    }

    void InterpolateSamples(const SurfaceSampleSet& set, const float* positions, const float* normals,
                            uint32_t strideFloats, float* outPositions, float* outNormals)
    {
        const bool withNormals = normals != nullptr && outNormals != nullptr;
        const size_t count = set.samples.size();
        for (size_t i = 0; i < count; ++i)
        {
            const SurfaceSample& s = set.samples[i];
            const float b0 = 1.0f - s.b1 - s.b2;
            const size_t o0 = size_t(s.vertex[0]) * strideFloats;
            const size_t o1 = size_t(s.vertex[1]) * strideFloats;
            const size_t o2 = size_t(s.vertex[2]) * strideFloats;

            for (uint32_t c = 0; c < 3; ++c)
            {
                outPositions[i * 3 + c] = b0 * positions[o0 + c] + s.b1 * positions[o1 + c] + s.b2 * positions[o2 + c];
            }

            if (withNormals)
            {
                float n[3];
                for (uint32_t c = 0; c < 3; ++c)
                {
                    n[c] = b0 * normals[o0 + c] + s.b1 * normals[o1 + c] + s.b2 * normals[o2 + c];
                }
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (uint32_t c = 0; c < 3; ++c)
                {
                    outNormals[i * 3 + c] = len > 0.0f ? n[c] / len : 0.0f;
                }
            }
        }
    }
}
