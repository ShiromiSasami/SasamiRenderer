#include "Renderer/Fluid/IsoSurfaceSelfTest.h"

#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/ParticleSplatGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        constexpr double PI = 3.14159265358979323846;

        // Fixed LCG so the particle sets are the same on every run.
        float NextUnit(uint32_t& state)
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
        }

        uint32_t FindRoot(std::vector<uint32_t>& parent, uint32_t i)
        {
            while (parent[i] != i) {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        }

        // Connected components of the vertices that at least one triangle uses.
        uint32_t CountComponents(const IsoSurfaceMesh& mesh)
        {
            std::vector<uint32_t> parent(mesh.VertexCount());
            std::iota(parent.begin(), parent.end(), 0u);
            std::vector<uint8_t> used(mesh.VertexCount(), 0);
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                for (size_t k = 0; k < 3; ++k) {
                    used[mesh.indices[i + k]] = 1;
                    const uint32_t a = FindRoot(parent, mesh.indices[i + k]);
                    const uint32_t b = FindRoot(parent, mesh.indices[i + (k + 1) % 3]);
                    if (a != b) {
                        parent[a] = b;
                    }
                }
            }
            uint32_t components = 0;
            for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
                if (used[v] && FindRoot(parent, v) == v) {
                    ++components;
                }
            }
            return components;
        }

        std::string Fail(const char* what, const std::ostringstream& detail)
        {
            return std::string("ERR ") + what + " " + detail.str();
        }

        struct SphereStats
        {
            uint32_t openEdges = 0;
            float volume = 0.0f;
            float area = 0.0f;
            uint32_t triangles = 0;
            float maxRadiusErr = 0.0f;
            float minOutward = 0.0f;
            float maxAttrErr = 0.0f;
        };

        SphereStats MeasureSphere(const IsoSurfaceMesh& mesh, const float (&center)[3], float expectedRadius, float expectedAttr)
        {
            SphereStats stats;
            stats.openEdges = CountOpenEdges(mesh);
            stats.volume = IsoSurfaceVolume(mesh);
            stats.area = IsoSurfaceArea(mesh);
            stats.triangles = mesh.TriangleCount();
            stats.minOutward = 1e30f;
            for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
                const float dx = mesh.positions[v * 3 + 0] - center[0];
                const float dy = mesh.positions[v * 3 + 1] - center[1];
                const float dz = mesh.positions[v * 3 + 2] - center[2];
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                stats.maxRadiusErr = (std::max)(stats.maxRadiusErr, std::fabs(dist - expectedRadius));
                const float outward = mesh.normals[v * 3 + 0] * dx + mesh.normals[v * 3 + 1] * dy + mesh.normals[v * 3 + 2] * dz;
                stats.minOutward = (std::min)(stats.minOutward, outward);
                stats.maxAttrErr = (std::max)(stats.maxAttrErr, std::fabs(mesh.attributes[v] - expectedAttr));
            }
            return stats;
        }
    }

    std::string RunIsoSurfaceSelfTest()
    {
        uint32_t checks = 0;
        std::ostringstream detail;
        detail << std::fixed << std::setprecision(5);

        // 1. Empty input.
        {
            ParticleSplatGrid grid;
            grid.Fit(nullptr, 0, 3, 1.0f, 0.1f, 256);
            IsoSurfaceMesh mesh;
            ExtractIsoSurface(grid, 0.5f, mesh);
            ++checks;
            if (grid.CellCount() != 0 || mesh.TriangleCount() != 0) {
                detail << "cells=" << grid.CellCount() << " triangles=" << mesh.TriangleCount();
                return Fail("empty input is not empty", detail);
            }
        }

        // 2. Single sphere against the analytic level set.
        constexpr float RADIUS = 1.0f;
        constexpr float ISO = 0.5f;
        constexpr float ATTRIBUTE = 0.7f;
        const float center[3] = { 0.3f, -0.2f, 0.1f };
        const double levelRadius = RADIUS * std::sqrt(1.0 - std::cbrt(static_cast<double>(ISO)));
        const double expectedVolume = 4.0 / 3.0 * PI * levelRadius * levelRadius * levelRadius;
        const double expectedArea = 4.0 * PI * levelRadius * levelRadius;
        std::string sphereSummary;
        {
            ParticleSplatGrid grid;
            grid.Fit(center, 1, 3, RADIUS, 0.05f, 256);
            grid.SplatAll(center, 1, 3, nullptr, RADIUS, nullptr, &ATTRIBUTE);
            IsoSurfaceMesh mesh;
            ExtractIsoSurface(grid, ISO, mesh);
            const float cellSize = grid.Desc().cellSize;
            const SphereStats s = MeasureSphere(mesh, center, static_cast<float>(levelRadius), ATTRIBUTE);
            const double volumeErr = std::fabs(s.volume - expectedVolume) / expectedVolume;
            const double areaErr = std::fabs(s.area - expectedArea) / expectedArea;
            detail << "openEdges=" << s.openEdges << " V=" << s.volume << " expectedV=" << expectedVolume
                   << " A=" << s.area << " expectedA=" << expectedArea << " triangles=" << s.triangles
                   << " maxRadiusErr=" << s.maxRadiusErr << " minOutward=" << s.minOutward << " maxAttrErr=" << s.maxAttrErr;
            ++checks;
            if (s.triangles == 0 || s.openEdges != 0) {
                return Fail("sphere mesh not closed", detail);
            }
            ++checks;
            if (!(s.volume > 0.0f) || !(volumeErr < 0.03)) {
                return Fail("sphere volume", detail);
            }
            ++checks;
            if (!(areaErr < 0.05)) {
                return Fail("sphere area", detail);
            }
            ++checks;
            if (!(s.maxRadiusErr < cellSize)) {
                return Fail("sphere vertex radius", detail);
            }
            ++checks;
            if (!(s.minOutward > 0.0f)) {
                return Fail("sphere normals not outward", detail);
            }
            ++checks;
            if (!(s.maxAttrErr < 1e-5f)) {
                return Fail("sphere attribute", detail);
            }
            std::ostringstream summary;
            summary << std::fixed << std::setprecision(4) << "V=" << s.volume << " expectedV=" << expectedVolume
                    << " A=" << s.area << " expectedA=" << expectedArea << " triangles=" << s.triangles;
            sphereSummary = summary.str();
        }

        // 3. Coarse sphere stays watertight.
        {
            ParticleSplatGrid grid;
            grid.Fit(center, 1, 3, RADIUS, 0.25f, 256);
            grid.SplatAll(center, 1, 3, nullptr, RADIUS, nullptr, &ATTRIBUTE);
            IsoSurfaceMesh mesh;
            ExtractIsoSurface(grid, ISO, mesh);
            const uint32_t openEdges = CountOpenEdges(mesh);
            const float volume = IsoSurfaceVolume(mesh);
            ++checks;
            if (mesh.TriangleCount() == 0 || openEdges != 0 || !(volume > 0.0f)) {
                detail.str("");
                detail << "triangles=" << mesh.TriangleCount() << " openEdges=" << openEdges << " V=" << volume;
                return Fail("coarse sphere", detail);
            }
        }

        // 4. A sphere cut by the grid border is closed by the zero padding.
        {
            const float origin[3] = { 0.0f, 0.0f, 0.0f };
            SplatGridDesc desc;
            desc.origin[0] = 0.0f;
            desc.origin[1] = -1.5f;
            desc.origin[2] = -1.5f;
            desc.cellSize = 0.1f;
            desc.dims[0] = 15;
            desc.dims[1] = 30;
            desc.dims[2] = 30;
            ParticleSplatGrid grid;
            grid.Reset(desc);
            grid.Splat(origin, RADIUS, 1.0f, 0.0f);
            IsoSurfaceMesh mesh;
            ExtractIsoSurface(grid, ISO, mesh);
            const uint32_t openEdges = CountOpenEdges(mesh);
            ++checks;
            if (mesh.TriangleCount() == 0 || openEdges != 0) {
                detail.str("");
                detail << "triangles=" << mesh.TriangleCount() << " openEdges=" << openEdges;
                return Fail("border sphere not closed", detail);
            }
        }

        // 5. Merging: near particles give one closed surface, far ones two.
        {
            const float closePair[6] = { -0.6f, 0.0f, 0.0f, 0.6f, 0.0f, 0.0f };
            const float farPair[6] = { -3.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f };
            ParticleSplatGrid grid;
            IsoSurfaceMesh mesh;
            grid.Fit(closePair, 2, 3, RADIUS, 0.1f, 256);
            grid.SplatAll(closePair, 2, 3, nullptr, RADIUS, nullptr, nullptr);
            ExtractIsoSurface(grid, 0.3f, mesh);
            const uint32_t openEdges = CountOpenEdges(mesh);
            const uint32_t nearComponents = CountComponents(mesh);
            ++checks;
            if (openEdges != 0 || nearComponents != 1) {
                detail.str("");
                detail << "openEdges=" << openEdges << " components=" << nearComponents;
                return Fail("merged particles", detail);
            }
            grid.Fit(farPair, 2, 3, RADIUS, 0.1f, 256);
            grid.SplatAll(farPair, 2, 3, nullptr, RADIUS, nullptr, nullptr);
            ExtractIsoSurface(grid, 0.3f, mesh);
            const uint32_t farComponents = CountComponents(mesh);
            ++checks;
            if (CountOpenEdges(mesh) != 0 || farComponents != 2) {
                detail.str("");
                detail << "openEdges=" << CountOpenEdges(mesh) << " components=" << farComponents;
                return Fail("separate particles", detail);
            }
        }

        // 6. SplatAll equals serial Splat bit for bit, and so do the extracted meshes.
        {
            constexpr uint32_t COUNT = 500;
            std::vector<float> positions(COUNT * 3);
            std::vector<float> radii(COUNT);
            std::vector<float> weights(COUNT, 1.0f);
            std::vector<float> attributes(COUNT);
            uint32_t state = 12345u;
            for (uint32_t i = 0; i < COUNT; ++i) {
                for (uint32_t k = 0; k < 3; ++k) {
                    positions[i * 3 + k] = NextUnit(state) * 4.0f - 2.0f;
                }
                radii[i] = 0.3f + NextUnit(state) * 0.3f;
                attributes[i] = NextUnit(state);
            }
            ParticleSplatGrid gridA;
            gridA.Fit(positions.data(), COUNT, 3, 0.6f, 0.1f, 256);
            gridA.SplatAll(positions.data(), COUNT, 3, radii.data(), 0.5f, weights.data(), attributes.data());
            ParticleSplatGrid gridB;
            gridB.Fit(positions.data(), COUNT, 3, 0.6f, 0.1f, 256);
            for (uint32_t i = 0; i < COUNT; ++i) {
                gridB.Splat(&positions[i * 3], radii[i], weights[i], attributes[i]);
            }
            const std::vector<float>& a = gridA.DensityData();
            const std::vector<float>& b = gridB.DensityData();
            ++checks;
            if (a.size() != b.size() || a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) != 0) {
                detail.str("");
                detail << "cells=" << a.size() << "/" << b.size();
                return Fail("SplatAll differs from serial Splat", detail);
            }
            IsoSurfaceMesh meshA;
            IsoSurfaceMesh meshB;
            ExtractIsoSurface(gridA, 0.3f, meshA);
            ExtractIsoSurface(gridB, 0.3f, meshB);
            ++checks;
            if (meshA.TriangleCount() == 0 || meshA.indices != meshB.indices || meshA.positions != meshB.positions) {
                detail.str("");
                detail << "trianglesA=" << meshA.TriangleCount() << " trianglesB=" << meshB.TriangleCount();
                return Fail("extracted meshes differ", detail);
            }
        }

        // 7. Attribute is the weighted mean of the splats on a cell.
        {
            const float position[3] = { 0.0f, 0.0f, 0.0f };
            ParticleSplatGrid grid;
            grid.Fit(position, 1, 3, RADIUS, 0.1f, 256);
            grid.Splat(position, RADIUS, 1.0f, 0.2f);
            grid.Splat(position, RADIUS, 1.0f, 0.8f);
            const SplatGridDesc& desc = grid.Desc();
            const uint32_t cx = static_cast<uint32_t>((position[0] - desc.origin[0]) / desc.cellSize);
            const uint32_t cy = static_cast<uint32_t>((position[1] - desc.origin[1]) / desc.cellSize);
            const uint32_t cz = static_cast<uint32_t>((position[2] - desc.origin[2]) / desc.cellSize);
            const float mean = grid.Attribute(cx, cy, cz);
            ++checks;
            if (!(std::fabs(mean - 0.5f) < 1e-6f)) {
                detail.str("");
                detail << "mean=" << mean;
                return Fail("attribute mean", detail);
            }
        }

        std::ostringstream out;
        out << "OK checks=" << checks << " " << sphereSummary;
        return out.str();
    }
}
