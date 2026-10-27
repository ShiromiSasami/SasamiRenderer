#include "Renderer/Voxel/GpuSolidVoxelizerSelfTest.h"

#include "Renderer/Voxel/CpuSolidVoxelizer.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/GpuSolidVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <random>
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
            uint64_t occupied = 0;
            std::string error;
            std::string failure;
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

        VoxelGridDesc MakeDesc(uint32_t dimX, uint32_t dimY, uint32_t dimZ,
                               float ox, float oy, float oz, float voxelSize)
        {
            VoxelGridDesc desc;
            desc.origin[0] = ox;
            desc.origin[1] = oy;
            desc.origin[2] = oz;
            desc.voxelSize = voxelSize;
            desc.dimX = dimX;
            desc.dimY = dimY;
            desc.dimZ = dimZ;
            return desc;
        }

        CaseResult RunCase(GpuSolidVoxelizer& gpu,
                           const char* name,
                           const VoxelGridDesc& desc,
                           const VoxelizerMeshInput& mesh)
        {
            CaseResult result;
            result.name = name;

            OccupancyBitGrid cpuGrid(desc.dimX, desc.dimY, desc.dimZ);
            VoxelizeSolidParity(desc, mesh, cpuGrid);

            OccupancyBitGrid gpuGrid;
            std::string error;
            if (!gpu.Voxelize(desc, mesh, gpuGrid, error))
            {
                result.error = std::string(name) + ": " + error;
                return result;
            }

            uint64_t diff = 0;
            uint32_t first[3]{};
            bool firstCpu = false;
            bool firstGpu = false;
            for (uint32_t z = 0; z < desc.dimZ; ++z)
            {
                for (uint32_t y = 0; y < desc.dimY; ++y)
                {
                    for (uint32_t x = 0; x < desc.dimX; ++x)
                    {
                        const bool cpuBit = cpuGrid.Test(x, y, z);
                        const bool gpuBit = gpuGrid.Test(x, y, z);
                        if (cpuBit != gpuBit)
                        {
                            if (diff == 0)
                            {
                                first[0] = x;
                                first[1] = y;
                                first[2] = z;
                                firstCpu = cpuBit;
                                firstGpu = gpuBit;
                            }
                            ++diff;
                        }
                    }
                }
            }

            result.occupied = cpuGrid.CountOccupied();
            result.ok = (diff == 0);
            if (!result.ok)
            {
                result.failure = std::string(name) + " diff=" + std::to_string(diff)
                    + " first=(" + std::to_string(first[0]) + "," + std::to_string(first[1]) + ","
                    + std::to_string(first[2]) + ") cpu=" + (firstCpu ? "1" : "0")
                    + " gpu=" + (firstGpu ? "1" : "0");
            }
            return result;
        }
    }

    std::string RunGpuSolidVoxelizerSelfTest(IRhiDevice& device)
    {
        try
        {
            static GpuSolidVoxelizer gpu;
            if (!gpu.IsInitialized())
            {
                std::string error;
                if (!gpu.Initialize(device, error))
                {
                    return "ERR gpu solid voxelizer: " + error;
                }
            }

            std::vector<CaseResult> results;

            // soup and soup_nonindexed
            {
                const VoxelGridDesc desc = MakeDesc(37, 29, 23, -3.0f, -2.0f, -1.5f, 0.25f);
                std::mt19937 rng(1);
                std::uniform_real_distribution<float> distX(desc.origin[0] - 1.0f,
                    desc.origin[0] + desc.dimX * desc.voxelSize + 1.0f);
                std::uniform_real_distribution<float> distY(desc.origin[1] - 1.0f,
                    desc.origin[1] + desc.dimY * desc.voxelSize + 1.0f);
                std::uniform_real_distribution<float> distZ(desc.origin[2] - 1.0f,
                    desc.origin[2] + desc.dimZ * desc.voxelSize + 1.0f);

                std::vector<float> positions;
                std::vector<uint32_t> indices;
                for (uint32_t i = 0; i < 300 * 3; ++i)
                {
                    positions.push_back(distX(rng));
                    positions.push_back(distY(rng));
                    positions.push_back(distZ(rng));
                    indices.push_back(i);
                }

                VoxelizerMeshInput indexed;
                indexed.positions = positions.data();
                indexed.vertexCount = static_cast<uint32_t>(positions.size() / 3);
                indexed.indices = indices.data();
                indexed.indexCount = static_cast<uint32_t>(indices.size());
                results.push_back(RunCase(gpu, "soup", desc, indexed));

                VoxelizerMeshInput nonIndexed;
                nonIndexed.positions = positions.data();
                nonIndexed.vertexCount = static_cast<uint32_t>(positions.size() / 3);
                nonIndexed.indices = nullptr;
                results.push_back(RunCase(gpu, "soup_nonindexed", desc, nonIndexed));
            }

            // lattice
            {
                const VoxelGridDesc desc = MakeDesc(45, 16, 16, 0.0f, 0.0f, 0.0f, 1.0f);
                std::mt19937 rng(2);
                const uint32_t dims[3] = { desc.dimX, desc.dimY, desc.dimZ };
                std::vector<float> positions;
                std::vector<uint32_t> indices;
                for (uint32_t i = 0; i < 200 * 3; ++i)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        std::uniform_int_distribution<int> half(-2, static_cast<int>(2 * (dims[axis] + 1)));
                        positions.push_back(static_cast<float>(half(rng)) * 0.5f);
                    }
                    indices.push_back(i);
                }

                VoxelizerMeshInput mesh;
                mesh.positions = positions.data();
                mesh.vertexCount = static_cast<uint32_t>(positions.size() / 3);
                mesh.indices = indices.data();
                mesh.indexCount = static_cast<uint32_t>(indices.size());
                results.push_back(RunCase(gpu, "lattice", desc, mesh));
            }

            // box_on_centres
            {
                const VoxelGridDesc desc = MakeDesc(33, 16, 16, 0.0f, 0.0f, 0.0f, 1.0f);
                const double noRot[3] = { 0.0, 0.0, 0.0 };
                const double axisC[3] = { 17.0, 7.5, 7.5 };
                const double axisH[3] = { 13.5, 5.0, 3.0 };
                const double rotC[3] = { 16.5, 8.0, 8.0 };
                const double rotH[3] = { 5.0, 3.0, 4.0 };
                const double rotE[3] = { 0.3, 0.5, 0.7 };

                std::vector<float> positions;
                std::vector<uint32_t> indices;
                AppendBox(MakeBox(axisC, axisH, noRot), positions, indices);
                AppendBox(MakeBox(rotC, rotH, rotE), positions, indices);

                VoxelizerMeshInput mesh;
                mesh.positions = positions.data();
                mesh.vertexCount = static_cast<uint32_t>(positions.size() / 3);
                mesh.indices = indices.data();
                mesh.indexCount = static_cast<uint32_t>(indices.size());
                results.push_back(RunCase(gpu, "box_on_centres", desc, mesh));
            }

            // big_triangle
            {
                const VoxelGridDesc desc = MakeDesc(40, 40, 40, 0.0f, 0.0f, 0.0f, 1.0f);
                const float positions[18] = {
                    -5.0f, -20.0f, -20.0f,
                    30.0f, 80.0f, -20.0f,
                    10.0f, -20.0f, 80.0f,
                    45.0f, -10.0f, 50.0f,
                    -10.0f, 60.0f, 30.0f,
                    20.0f, 50.0f, -15.0f };
                const uint32_t indices[6] = { 0, 1, 2, 3, 4, 5 };

                VoxelizerMeshInput mesh;
                mesh.positions = positions;
                mesh.vertexCount = 6;
                mesh.indices = indices;
                mesh.indexCount = 6;
                results.push_back(RunCase(gpu, "big_triangle", desc, mesh));
            }

            for (const CaseResult& result : results)
            {
                if (!result.error.empty())
                {
                    return "ERR gpu solid voxelizer: " + result.error;
                }
            }

            std::string failures;
            for (const CaseResult& result : results)
            {
                if (!result.ok)
                {
                    failures += (failures.empty() ? "" : "; ") + result.failure;
                }
            }
            if (!failures.empty())
            {
                return "ERR gpu solid voxelizer: " + failures;
            }

            std::string report = "OK gpu solid voxelizer bit-identical ("
                + std::to_string(results.size()) + " cases)";
            for (const CaseResult& result : results)
            {
                report += " " + result.name + "=" + std::to_string(result.occupied);
            }
            return report;
        }
        catch (const std::exception& e)
        {
            return std::string("ERR gpu solid voxelizer: exception: ") + e.what();
        }
    }
}
