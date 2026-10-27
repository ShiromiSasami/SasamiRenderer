#include "Renderer/Fluid/IsoSurfaceMesh.h"

#include "Renderer/Fluid/ParticleSplatGrid.h"

#include "Foundation/Jobs/ParallelFor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        constexpr float MIN_NORMAL_LENGTH = 1e-20f;
        constexpr int TETRAHEDRA_PER_CUBE = 6;
        // Cube layers (along z) per parallel slab. Each slab builds its own mesh; ExtractSlabs joins them.
        constexpr int SLAB_CUBE_LAYERS = 4;
        // Edge key bit for an offset along z (offsetCode bit 2, see EdgeVertex).
        constexpr uint64_t KEY_OFFSET_Z_BIT = 4u;

        // Kuhn split (Freudenthal): one tetrahedron per permutation (a, b, c) of the axes, along the path
        // corner 0 -> e_a -> e_a + e_b -> corner 7. Entries are cube corner codes (bit 0 x, bit 1 y, bit 2 z).
        // The path is monotone, so for any two vertices the earlier one is the lower lattice endpoint.
        constexpr std::array<std::array<int, 4>, TETRAHEDRA_PER_CUBE> KUHN_TETRAHEDRA = { {
            { 0, 1, 3, 7 }, { 0, 1, 5, 7 }, { 0, 2, 3, 7 }, { 0, 2, 6, 7 }, { 0, 4, 5, 7 }, { 0, 4, 6, 7 },
        } };

        class IsoSurfaceBuilder
        {
        public:
            // grid supplies the attributes and may be null (then every attribute is 0).
            IsoSurfaceBuilder(const SplatGridDesc& desc, const float* density, const ParticleSplatGrid* grid,
                              float iso, float outsideValue, IsoSurfaceMesh& mesh)
                : m_grid(grid)
                , m_density(density)
                , m_desc(desc)
                , m_iso(iso)
                , m_outside(outsideValue)
                , m_mesh(mesh)
            {
                m_dims[0] = static_cast<int>(m_desc.dims[0]);
                m_dims[1] = static_cast<int>(m_desc.dims[1]);
                m_dims[2] = static_cast<int>(m_desc.dims[2]);
            }

            // Cubes whose lower z is in [kBegin, kEnd); the whole grid is [-1, dims z).
            void Run(int kBegin, int kEnd)
            {
                for (int k = kBegin; k < kEnd; ++k)
                {
                    for (int j = -1; j < m_dims[1]; ++j)
                    {
                        for (int i = -1; i < m_dims[0]; ++i)
                        {
                            ProcessCube(i, j, k);
                        }
                    }
                }
            }

            // Edge key of each vertex in creation order (lattice position of the lower endpoint + offset code).
            const std::vector<uint64_t>& VertexKeys() const { return m_vertexKeys; }

        private:
            float Value(int x, int y, int z) const
            {
                if (x < 0 || y < 0 || z < 0 || x >= m_dims[0] || y >= m_dims[1] || z >= m_dims[2])
                {
                    return m_outside;
                }
                return m_density[(size_t(z) * m_desc.dims[1] + size_t(y)) * m_desc.dims[0] + size_t(x)];
            }

            float AttributeAt(int x, int y, int z) const
            {
                if (m_grid == nullptr || x < 0 || y < 0 || z < 0 || x >= m_dims[0] || y >= m_dims[1] || z >= m_dims[2])
                {
                    return 0.0f;
                }
                return m_grid->Attribute(uint32_t(x), uint32_t(y), uint32_t(z));
            }

            void Gradient(int x, int y, int z, float outGradient[3]) const
            {
                const float inv = 1.0f / (2.0f * m_desc.cellSize);
                outGradient[0] = (Value(x + 1, y, z) - Value(x - 1, y, z)) * inv;
                outGradient[1] = (Value(x, y + 1, z) - Value(x, y - 1, z)) * inv;
                outGradient[2] = (Value(x, y, z + 1) - Value(x, y, z - 1)) * inv;
            }

            // Vertex on the lattice edge from `lower` to lower + offset(offsetCode); created on first use.
            uint32_t EdgeVertex(const int lower[3], int offsetCode)
            {
                const uint64_t strideX = uint64_t(m_dims[0] + 2);
                const uint64_t strideY = uint64_t(m_dims[1] + 2);
                const uint64_t padded =
                    (uint64_t(lower[2] + 1) * strideY + uint64_t(lower[1] + 1)) * strideX + uint64_t(lower[0] + 1);
                const uint64_t key = (padded << 3) | uint64_t(offsetCode);
                const auto found = m_edgeVertices.find(key);
                if (found != m_edgeVertices.end())
                {
                    return found->second;
                }

                const int upper[3] = { lower[0] + (offsetCode & 1), lower[1] + ((offsetCode >> 1) & 1),
                                       lower[2] + ((offsetCode >> 2) & 1) };
                const float v0 = Value(lower[0], lower[1], lower[2]);
                const float v1 = Value(upper[0], upper[1], upper[2]);
                const float denom = v1 - v0;
                float t = denom != 0.0f ? (m_iso - v0) / denom : 0.5f;
                t = (std::min)((std::max)(t, 0.0f), 1.0f);

                float a0 = AttributeAt(lower[0], lower[1], lower[2]);
                float a1 = AttributeAt(upper[0], upper[1], upper[2]);
                if (v0 <= 0.0f)
                {
                    a0 = a1;
                }
                else if (v1 <= 0.0f)
                {
                    a1 = a0;
                }

                float g0[3];
                float g1[3];
                Gradient(lower[0], lower[1], lower[2], g0);
                Gradient(upper[0], upper[1], upper[2], g1);
                float normal[3];
                float length = 0.0f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    normal[axis] = -(g0[axis] + (g1[axis] - g0[axis]) * t);
                    length += normal[axis] * normal[axis];
                }
                length = std::sqrt(length);
                if (length < MIN_NORMAL_LENGTH)
                {
                    normal[0] = 0.0f;
                    normal[1] = 1.0f;
                    normal[2] = 0.0f;
                }
                else
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        normal[axis] /= length;
                    }
                }

                for (int axis = 0; axis < 3; ++axis)
                {
                    const float p0 = m_desc.origin[axis] + (float(lower[axis]) + 0.5f) * m_desc.cellSize;
                    const float p1 = m_desc.origin[axis] + (float(upper[axis]) + 0.5f) * m_desc.cellSize;
                    m_mesh.positions.push_back(p0 + (p1 - p0) * t);
                }
                for (int axis = 0; axis < 3; ++axis)
                {
                    m_mesh.normals.push_back(normal[axis]);
                }
                m_mesh.attributes.push_back(a0 + (a1 - a0) * t);

                const uint32_t index = m_mesh.VertexCount() - 1;
                m_edgeVertices.emplace(key, index);
                m_vertexKeys.push_back(key);
                return index;
            }

            // Appends a triangle wound counter-clockwise seen from outside; `toInside` points toward higher density.
            void EmitTriangle(uint32_t a, uint32_t b, uint32_t c, const float toInside[3])
            {
                if (a == b || b == c || a == c)
                {
                    return;
                }
                const float* pa = &m_mesh.positions[size_t(a) * 3];
                const float* pb = &m_mesh.positions[size_t(b) * 3];
                const float* pc = &m_mesh.positions[size_t(c) * 3];
                const float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
                const float e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
                const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                     e1[0] * e2[1] - e1[1] * e2[0] };
                const float alongInside = n[0] * toInside[0] + n[1] * toInside[1] + n[2] * toInside[2];
                if (alongInside > 0.0f)
                {
                    std::swap(b, c);
                }
                m_mesh.indices.push_back(a);
                m_mesh.indices.push_back(b);
                m_mesh.indices.push_back(c);
            }

            void ProcessCube(int cubeX, int cubeY, int cubeZ)
            {
                const int base[3] = { cubeX, cubeY, cubeZ };
                float cornerValue[8];
                bool any = false;
                bool all = true;
                for (int c = 0; c < 8; ++c)
                {
                    cornerValue[c] = Value(cubeX + (c & 1), cubeY + ((c >> 1) & 1), cubeZ + ((c >> 2) & 1));
                    const bool inside = cornerValue[c] > m_iso;
                    any = any || inside;
                    all = all && inside;
                }
                if (!any || all)
                {
                    return;
                }

                for (const std::array<int, 4>& tetra : KUHN_TETRAHEDRA)
                {
                    ProcessTetrahedron(base, tetra, cornerValue);
                }
            }

            void ProcessTetrahedron(const int base[3], const std::array<int, 4>& tetra, const float cornerValue[8])
            {
                int insideList[4];
                int outsideList[4];
                int insideCount = 0;
                int outsideCount = 0;
                for (int v = 0; v < 4; ++v)
                {
                    if (cornerValue[tetra[v]] > m_iso)
                    {
                        insideList[insideCount++] = v;
                    }
                    else
                    {
                        outsideList[outsideCount++] = v;
                    }
                }
                if (insideCount == 0 || insideCount == 4)
                {
                    return;
                }

                float toInside[3] = { 0.0f, 0.0f, 0.0f };
                for (int n = 0; n < insideCount; ++n)
                {
                    AddCornerPosition(base, tetra[insideList[n]], 1.0f / float(insideCount), toInside);
                }
                for (int n = 0; n < outsideCount; ++n)
                {
                    AddCornerPosition(base, tetra[outsideList[n]], -1.0f / float(outsideCount), toInside);
                }

                if (insideCount == 1 || outsideCount == 1)
                {
                    const bool loneInside = insideCount == 1;
                    const int lone = loneInside ? insideList[0] : outsideList[0];
                    const int* others = loneInside ? outsideList : insideList;
                    const uint32_t e0 = TetraEdgeVertex(base, tetra, lone, others[0]);
                    const uint32_t e1 = TetraEdgeVertex(base, tetra, lone, others[1]);
                    const uint32_t e2 = TetraEdgeVertex(base, tetra, lone, others[2]);
                    EmitTriangle(e0, e1, e2, toInside);
                    return;
                }

                const int a = insideList[0];
                const int b = insideList[1];
                const int c = outsideList[0];
                const int d = outsideList[1];
                const uint32_t ac = TetraEdgeVertex(base, tetra, a, c);
                const uint32_t bc = TetraEdgeVertex(base, tetra, b, c);
                const uint32_t bd = TetraEdgeVertex(base, tetra, b, d);
                const uint32_t ad = TetraEdgeVertex(base, tetra, a, d);
                EmitTriangle(ac, bc, bd, toInside);
                EmitTriangle(ac, bd, ad, toInside);
            }

            void AddCornerPosition(const int base[3], int cornerCode, float weight, float accum[3]) const
            {
                accum[0] += weight * (float(base[0] + (cornerCode & 1)) + 0.5f) * m_desc.cellSize;
                accum[1] += weight * (float(base[1] + ((cornerCode >> 1) & 1)) + 0.5f) * m_desc.cellSize;
                accum[2] += weight * (float(base[2] + ((cornerCode >> 2) & 1)) + 0.5f) * m_desc.cellSize;
            }

            // Tetra vertices are in path order, so the lower of two is the one with the smaller tetra index.
            uint32_t TetraEdgeVertex(const int base[3], const std::array<int, 4>& tetra, int vertexA, int vertexB)
            {
                const int first = (std::min)(vertexA, vertexB);
                const int second = (std::max)(vertexA, vertexB);
                const int lowerCode = tetra[first];
                const int offsetCode = tetra[second] ^ lowerCode;
                const int lower[3] = { base[0] + (lowerCode & 1), base[1] + ((lowerCode >> 1) & 1),
                                       base[2] + ((lowerCode >> 2) & 1) };
                return EdgeVertex(lower, offsetCode);
            }

            const ParticleSplatGrid* m_grid;
            const float* m_density;
            const SplatGridDesc& m_desc;
            float m_iso;
            float m_outside;
            IsoSurfaceMesh& m_mesh;
            int m_dims[3] = { 0, 0, 0 };
            std::unordered_map<uint64_t, uint32_t> m_edgeVertices;
            std::vector<uint64_t> m_vertexKeys;
        };

        // Lower-endpoint z of the edge an IsoSurfaceBuilder key names (undoes the padding in EdgeVertex).
        int KeyLowerZ(uint64_t key, const SplatGridDesc& desc)
        {
            const uint64_t padded = key >> 3;
            const uint64_t plane = uint64_t(desc.dims[0] + 2) * uint64_t(desc.dims[1] + 2);
            return static_cast<int>(padded / plane) - 1;
        }

        // Runs the builder over z slabs of cube layers in parallel and joins the slab meshes. Two neighbouring
        // slabs share only the edges lying in the plane between them (lower z == the later slab's first layer,
        // no z offset); the later slab reuses the earlier slab's vertex for those. The earlier slab is also the
        // one that created such a vertex first in the serial order, and every other vertex keeps its slab order,
        // so the result is identical to one serial pass over all layers (same vertices, indices and order).
        void ExtractSlabs(const SplatGridDesc& desc, const float* density, const ParticleSplatGrid* grid, float iso,
                          float outsideValue, IsoSurfaceMesh& outMesh)
        {
            const int layerBegin = -1;
            const int layerEnd = static_cast<int>(desc.dims[2]);
            const uint32_t numSlabs = uint32_t((layerEnd - layerBegin + SLAB_CUBE_LAYERS - 1) / SLAB_CUBE_LAYERS);
            std::vector<IsoSurfaceMesh> slabMeshes(numSlabs);
            std::vector<std::vector<uint64_t>> slabKeys(numSlabs);
            Foundation::ParallelFor(numSlabs, [&](uint32_t s)
            {
                const int kBegin = layerBegin + int(s) * SLAB_CUBE_LAYERS;
                const int kEnd = (std::min)(kBegin + SLAB_CUBE_LAYERS, layerEnd);
                IsoSurfaceBuilder builder(desc, density, grid, iso, outsideValue, slabMeshes[s]);
                builder.Run(kBegin, kEnd);
                slabKeys[s] = builder.VertexKeys();
            });

            size_t totalVertices = 0;
            size_t totalIndices = 0;
            for (const IsoSurfaceMesh& slab : slabMeshes)
            {
                totalVertices += slab.VertexCount();
                totalIndices += slab.indices.size();
            }
            outMesh.positions.reserve(totalVertices * 3);
            outMesh.normals.reserve(totalVertices * 3);
            outMesh.attributes.reserve(totalVertices);
            outMesh.indices.reserve(totalIndices);

            std::unordered_map<uint64_t, uint32_t> sharedPlane;     // keys in the plane below the current slab
            std::unordered_map<uint64_t, uint32_t> nextPlane;
            std::vector<uint32_t> remap;
            for (uint32_t s = 0; s < numSlabs; ++s)
            {
                const IsoSurfaceMesh& slab = slabMeshes[s];
                const std::vector<uint64_t>& keys = slabKeys[s];
                const int kBegin = layerBegin + int(s) * SLAB_CUBE_LAYERS;
                const int kEnd = (std::min)(kBegin + SLAB_CUBE_LAYERS, layerEnd);
                remap.resize(slab.VertexCount());
                nextPlane.clear();
                for (uint32_t v = 0; v < slab.VertexCount(); ++v)
                {
                    const uint64_t key = keys[v];
                    const bool inPlane = (key & KEY_OFFSET_Z_BIT) == 0;
                    const int lowerZ = KeyLowerZ(key, desc);
                    if (s > 0 && inPlane && lowerZ == kBegin)
                    {
                        const auto found = sharedPlane.find(key);
                        if (found != sharedPlane.end())
                        {
                            remap[v] = found->second;
                            continue;
                        }
                    }
                    const uint32_t index = outMesh.VertexCount();
                    outMesh.positions.insert(outMesh.positions.end(), &slab.positions[size_t(v) * 3],
                                             &slab.positions[size_t(v) * 3] + 3);
                    outMesh.normals.insert(outMesh.normals.end(), &slab.normals[size_t(v) * 3],
                                           &slab.normals[size_t(v) * 3] + 3);
                    outMesh.attributes.push_back(slab.attributes[v]);
                    remap[v] = index;
                    if (inPlane && lowerZ == kEnd)
                    {
                        nextPlane.emplace(key, index);
                    }
                }
                for (uint32_t i : slab.indices)
                {
                    outMesh.indices.push_back(remap[i]);
                }
                std::swap(sharedPlane, nextPlane);
            }
        }
    }

    // iso must be > 0: the zero padding around the grid has to count as outside for surfaces to close.
    void ExtractIsoSurface(const ParticleSplatGrid& grid, float iso, IsoSurfaceMesh& outMesh)
    {
        outMesh.Clear();
        const SplatGridDesc& desc = grid.Desc();
        if (!(iso > 0.0f) || desc.dims[0] == 0 || desc.dims[1] == 0 || desc.dims[2] == 0)
        {
            return;
        }
        ExtractSlabs(desc, grid.DensityData().data(), &grid, iso, 0.0f, outMesh);
    }

    void ExtractIsoSurface(const SplatGridDesc& desc, const float* density, float iso, float outsideValue,
                           IsoSurfaceMesh& outMesh)
    {
        outMesh.Clear();
        if (density == nullptr || !(iso > outsideValue) || desc.dims[0] == 0 || desc.dims[1] == 0 || desc.dims[2] == 0)
        {
            return;
        }
        ExtractSlabs(desc, density, nullptr, iso, outsideValue, outMesh);
    }

    float IsoSurfaceVolume(const IsoSurfaceMesh& mesh)
    {
        double volume = 0.0;
        for (uint32_t t = 0; t < mesh.TriangleCount(); ++t)
        {
            const float* a = &mesh.positions[size_t(mesh.indices[t * 3 + 0]) * 3];
            const float* b = &mesh.positions[size_t(mesh.indices[t * 3 + 1]) * 3];
            const float* c = &mesh.positions[size_t(mesh.indices[t * 3 + 2]) * 3];
            const double cx = double(b[1]) * c[2] - double(b[2]) * c[1];
            const double cy = double(b[2]) * c[0] - double(b[0]) * c[2];
            const double cz = double(b[0]) * c[1] - double(b[1]) * c[0];
            volume += (double(a[0]) * cx + double(a[1]) * cy + double(a[2]) * cz) / 6.0;
        }
        return static_cast<float>(volume);
    }

    float IsoSurfaceArea(const IsoSurfaceMesh& mesh)
    {
        double area = 0.0;
        for (uint32_t t = 0; t < mesh.TriangleCount(); ++t)
        {
            const float* a = &mesh.positions[size_t(mesh.indices[t * 3 + 0]) * 3];
            const float* b = &mesh.positions[size_t(mesh.indices[t * 3 + 1]) * 3];
            const float* c = &mesh.positions[size_t(mesh.indices[t * 3 + 2]) * 3];
            const double e1[3] = { double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2] };
            const double e2[3] = { double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2] };
            const double cx = e1[1] * e2[2] - e1[2] * e2[1];
            const double cy = e1[2] * e2[0] - e1[0] * e2[2];
            const double cz = e1[0] * e2[1] - e1[1] * e2[0];
            area += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        }
        return static_cast<float>(area);
    }

    uint32_t CountOpenEdges(const IsoSurfaceMesh& mesh)
    {
        std::unordered_map<uint64_t, uint32_t> useCount;
        useCount.reserve(mesh.indices.size());
        for (uint32_t t = 0; t < mesh.TriangleCount(); ++t)
        {
            for (uint32_t e = 0; e < 3; ++e)
            {
                const uint32_t i0 = mesh.indices[t * 3 + e];
                const uint32_t i1 = mesh.indices[t * 3 + (e + 1) % 3];
                const uint64_t key = (uint64_t((std::min)(i0, i1)) << 32) | uint64_t((std::max)(i0, i1));
                ++useCount[key];
            }
        }
        uint32_t open = 0;
        for (const auto& entry : useCount)
        {
            if (entry.second != 2)
            {
                ++open;
            }
        }
        return open;
    }
}
