#include "Renderer/Voxel/SolidVoxelizerSelfTest.h"

#include "Renderer/Voxel/CpuSolidVoxelizer.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        struct CaseResult
        {
            std::string name;
            bool ok = false;
            std::string line;
        };

        struct TestBox
        {
            double center[3]{};
            double half[3]{};
            double rot[3][3]{};   // local -> world, row-major
        };

        // R = Rz * Ry * Rx
        TestBox MakeBox(const double center[3], const double half[3], const double euler[3])
        {
            TestBox box;
            const double cx = std::cos(euler[0]), sx = std::sin(euler[0]);
            const double cy = std::cos(euler[1]), sy = std::sin(euler[1]);
            const double cz = std::cos(euler[2]), sz = std::sin(euler[2]);
            const double rx[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
            const double ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
            const double rz[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
            double ryx[3][3]{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k)
                        ryx[r][c] += ry[r][k] * rx[k][c];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k)
                        box.rot[r][c] += rz[r][k] * ryx[k][c];
            for (int i = 0; i < 3; ++i)
            {
                box.center[i] = center[i];
                box.half[i] = half[i];
            }
            return box;
        }

        // Appends 8 corners and 12 triangles with outward winding.
        void AppendBox(const TestBox& box, std::vector<float>& positions, std::vector<uint32_t>& indices)
        {
            const uint32_t base = static_cast<uint32_t>(positions.size() / 3);
            for (uint32_t i = 0; i < 8; ++i)
            {
                const double local[3] = {
                    (i & 1) ? box.half[0] : -box.half[0],
                    (i & 2) ? box.half[1] : -box.half[1],
                    (i & 4) ? box.half[2] : -box.half[2] };
                for (int r = 0; r < 3; ++r)
                {
                    const double w = box.center[r]
                        + box.rot[r][0] * local[0] + box.rot[r][1] * local[1] + box.rot[r][2] * local[2];
                    positions.push_back(static_cast<float>(w));
                }
            }
            static const uint32_t FACE_INDICES[36] = {
                0, 2, 1, 1, 2, 3,   // -z
                4, 5, 6, 5, 7, 6,   // +z
                0, 4, 2, 2, 4, 6,   // -x
                1, 3, 5, 3, 7, 5,   // +x
                0, 1, 4, 1, 5, 4,   // -y
                2, 6, 3, 3, 6, 7 }; // +y
            for (uint32_t index : FACE_INDICES)
                indices.push_back(base + index);
        }

        void FlipWinding(std::vector<uint32_t>& indices)
        {
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
                std::swap(indices[i + 1], indices[i + 2]);
        }

        bool InsideBox(const TestBox& box, const double p[3])
        {
            const double d[3] = { p[0] - box.center[0], p[1] - box.center[1], p[2] - box.center[2] };
            for (int axis = 0; axis < 3; ++axis)
            {
                const double local = box.rot[0][axis] * d[0] + box.rot[1][axis] * d[1] + box.rot[2][axis] * d[2];
                if (!(std::fabs(local) < box.half[axis]))
                {
                    return false;
                }
            }
            return true;
        }

        // Expected occupancy: inside every box in `solids` and outside every box in `cavities`.
        CaseResult RunCase(const char* name,
                           const VoxelGridDesc& desc,
                           const std::vector<float>& positions,
                           const std::vector<uint32_t>& indices,
                           const std::vector<TestBox>& solids,
                           const std::vector<TestBox>& cavities)
        {
            CaseResult result;
            result.name = name;

            VoxelizerMeshInput mesh;
            mesh.positions = positions.data();
            mesh.vertexCount = static_cast<uint32_t>(positions.size() / 3);
            mesh.indices = indices.data();
            mesh.indexCount = static_cast<uint32_t>(indices.size());

            OccupancyBitGrid grid(desc.dimX, desc.dimY, desc.dimZ);
            VoxelizeSolidParity(desc, mesh, grid);

            uint64_t mismatches = 0;
            uint64_t expectedCount = 0;
            uint64_t gotCount = 0;
            uint32_t first[3]{};
            for (uint32_t z = 0; z < desc.dimZ; ++z)
            {
                for (uint32_t y = 0; y < desc.dimY; ++y)
                {
                    for (uint32_t x = 0; x < desc.dimX; ++x)
                    {
                        const double p[3] = {
                            desc.origin[0] + (x + 0.5) * desc.voxelSize,
                            desc.origin[1] + (y + 0.5) * desc.voxelSize,
                            desc.origin[2] + (z + 0.5) * desc.voxelSize };
                        bool expected = false;
                        for (const TestBox& solid : solids)
                        {
                            expected = expected || InsideBox(solid, p);
                        }
                        for (const TestBox& cavity : cavities)
                        {
                            expected = expected && !InsideBox(cavity, p);
                        }
                        const bool got = grid.Test(x, y, z);
                        expectedCount += expected ? 1 : 0;
                        gotCount += got ? 1 : 0;
                        if (expected != got)
                        {
                            if (mismatches == 0)
                            {
                                first[0] = x;
                                first[1] = y;
                                first[2] = z;
                            }
                            ++mismatches;
                        }
                    }
                }
            }

            result.ok = (mismatches == 0);
            if (result.ok)
            {
                result.line = std::string(" OK ") + name + ": occupied=" + std::to_string(expectedCount);
            }
            else
            {
                result.line = std::string(" ERR ") + name + ": mismatches=" + std::to_string(mismatches)
                    + " (expected " + std::to_string(expectedCount) + ", got " + std::to_string(gotCount)
                    + ") first=(" + std::to_string(first[0]) + "," + std::to_string(first[1]) + ","
                    + std::to_string(first[2]) + ")";
            }
            return result;
        }
    }

    std::string RunSolidVoxelizerSelfTest()
    {
        try
        {
            VoxelGridDesc desc;
            desc.origin[0] = -16.0f;
            desc.origin[1] = -16.0f;
            desc.origin[2] = -16.0f;
            desc.voxelSize = 1.0f;
            desc.dimX = desc.dimY = desc.dimZ = 32;

            const double noRot[3] = { 0.0, 0.0, 0.0 };
            std::vector<CaseResult> results;

            {
                const double c[3] = { 0.37, -0.21, 0.13 };
                const double h[3] = { 7.3, 5.1, 9.6 };
                const TestBox box = MakeBox(c, h, noRot);
                std::vector<float> positions;
                std::vector<uint32_t> indices;
                AppendBox(box, positions, indices);
                results.push_back(RunCase("axis box", desc, positions, indices, { box }, {}));
            }

            const double rotC[3] = { 1.13, 0.71, -0.42 };
            const double rotH[3] = { 8.2, 4.4, 6.3 };
            const double rotE[3] = { 0.41, 0.73, 0.29 };
            const TestBox rotBox = MakeBox(rotC, rotH, rotE);
            {
                std::vector<float> positions;
                std::vector<uint32_t> indices;
                AppendBox(rotBox, positions, indices);
                results.push_back(RunCase("rotated box", desc, positions, indices, { rotBox }, {}));
            }

            {
                const double oc[3] = { 0.17, 0.23, 0.31 };
                const double oh[3] = { 11.4, 10.2, 9.7 };
                const double oe[3] = { 0.2, 0.5, 0.1 };
                const double ic[3] = { 0.9, -0.4, 0.6 };
                const double ih[3] = { 4.3, 3.7, 5.1 };
                const double ie[3] = { 0.6, 0.1, 0.35 };
                const TestBox outer = MakeBox(oc, oh, oe);
                const TestBox inner = MakeBox(ic, ih, ie);
                std::vector<float> positions;
                std::vector<uint32_t> indices;
                AppendBox(outer, positions, indices);
                AppendBox(inner, positions, indices);
                results.push_back(RunCase("nested cavity", desc, positions, indices, { outer }, { inner }));
            }

            {
                const double c[3] = { 12.37, 3.21, -9.13 };
                const double h[3] = { 9.3, 6.2, 5.4 };
                const TestBox box = MakeBox(c, h, noRot);
                std::vector<float> positions;
                std::vector<uint32_t> indices;
                AppendBox(box, positions, indices);
                results.push_back(RunCase("partly outside grid", desc, positions, indices, { box }, {}));
            }

            {
                std::vector<float> positions;
                std::vector<uint32_t> indices;
                AppendBox(rotBox, positions, indices);
                FlipWinding(indices);
                results.push_back(RunCase("rotated box winding flipped", desc, positions, indices, { rotBox }, {}));
            }

            size_t failed = 0;
            std::string body;
            for (const CaseResult& result : results)
            {
                failed += result.ok ? 0 : 1;
                body += "\n" + result.line;
            }
            const std::string header = (failed == 0)
                ? "OK solid voxelizer self-test (" + std::to_string(results.size()) + " cases)"
                : "ERR solid voxelizer self-test: " + std::to_string(failed) + "/"
                    + std::to_string(results.size()) + " failed";
            return header + body;
        }
        catch (const std::exception& e)
        {
            return std::string("ERR solid voxelizer self-test exception: ") + e.what();
        }
    }
}
