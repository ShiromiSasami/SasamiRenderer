#include "Renderer/Voxel/VoxelizerSelfTest.h"

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/GpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/VoxelRegion.h"

#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
            std::string line;
        };

        std::string FormatMs(double ms)
        {
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%.2f", ms);
            return buffer;
        }

        CaseResult RunCase(GpuSurfaceVoxelizer& gpu,
                           const char* name,
                           const VoxelGridDesc& desc,
                           const VoxelizerMeshInput& mesh)
        {
            CaseResult result;
            result.name = name;

            OccupancyBitGrid cpuGrid(desc.dimX, desc.dimY, desc.dimZ);
            VoxelizeSurface(desc, mesh, cpuGrid);

            OccupancyBitGrid gpuGrid;
            std::string error;
            const auto start = std::chrono::steady_clock::now();
            const bool voxelized = gpu.Voxelize(desc, mesh, gpuGrid, error);
            const auto end = std::chrono::steady_clock::now();
            if (!voxelized)
            {
                result.line = std::string(" ERR ") + name + ": " + error;
                return result;
            }
            const double gpuMs = std::chrono::duration<double, std::milli>(end - start).count();

            const std::vector<uint32_t>& cpuWords = cpuGrid.Words();
            const std::vector<uint32_t>& gpuWords = gpuGrid.Words();
            uint64_t diffWords = 0;
            uint64_t diffBits = 0;
            if (cpuWords.size() != gpuWords.size())
            {
                result.line = std::string(" ERR ") + name + ": word count cpu="
                    + std::to_string(cpuWords.size()) + " gpu=" + std::to_string(gpuWords.size());
                return result;
            }
            for (size_t i = 0; i < cpuWords.size(); ++i)
            {
                const uint32_t diff = cpuWords[i] ^ gpuWords[i];
                if (diff != 0)
                {
                    ++diffWords;
                    diffBits += std::bitset<32>(diff).count();
                }
            }

            result.ok = (diffBits == 0);
            result.line = std::string(" ") + name
                + ": cpu=" + std::to_string(cpuGrid.CountOccupied())
                + " gpu=" + std::to_string(gpuGrid.CountOccupied())
                + " diffWords=" + std::to_string(diffWords)
                + " diffBits=" + std::to_string(diffBits)
                + " gpuMs=" + FormatMs(gpuMs);
            return result;
        }

        // 8 corners and 12 triangles of an axis-aligned cube spanning [-h, h]^3.
        void BuildCube(float h, std::vector<float>& positions, std::vector<uint32_t>& indices)
        {
            positions.clear();
            for (uint32_t i = 0; i < 8; ++i)
            {
                positions.push_back((i & 1) ? h : -h);
                positions.push_back((i & 2) ? h : -h);
                positions.push_back((i & 4) ? h : -h);
            }
            indices = {
                0, 2, 1, 1, 2, 3,   // z-
                4, 5, 6, 5, 7, 6,   // z+
                0, 1, 4, 1, 5, 4,   // y-
                2, 6, 3, 3, 6, 7,   // y+
                0, 4, 2, 2, 4, 6,   // x-
                1, 3, 5, 3, 7, 5    // x+
            };
        }

        // CPU-only: region voxelization must match the full grid bit for bit, both as a sub-box
        // (Test) and after being written back over the cleared box (WriteRegion, word for word).
        CaseResult RunRegionCase(const char* name,
                                 const VoxelGridDesc& desc,
                                 const VoxelizerMeshInput& mesh)
        {
            CaseResult result;
            result.name = name;

            OccupancyBitGrid fullGrid(desc.dimX, desc.dimY, desc.dimZ);
            VoxelizeSurface(desc, mesh, fullGrid);

            const uint32_t dx = desc.dimX;
            const uint32_t dy = desc.dimY;
            const uint32_t dz = desc.dimZ;
            VoxelRegion regions[4];
            regions[0] = FullVoxelRegion(desc);
            regions[1] = { { dx / 4, dy / 4, dz / 4 }, { dx / 2, dy / 2, dz / 2 } };
            regions[2] = { { dx - dx / 3, 0, dz / 5 }, { dx / 3, dy / 2, dz - dz / 5 } };
            regions[3] = { { dx / 2, dy / 2 + 1, dz / 2 - 1 }, { 1, 1, 1 } };

            uint64_t diffBits = 0;
            uint64_t diffWords = 0;
            uint64_t regionVoxels = 0;
            for (const VoxelRegion& region : regions)
            {
                OccupancyBitGrid regionGrid(region.size[0], region.size[1], region.size[2]);
                VoxelizeSurfaceRegion(desc, region, mesh, regionGrid);
                regionVoxels += regionGrid.CountOccupied();

                for (uint32_t z = 0; z < region.size[2]; ++z)
                {
                    for (uint32_t y = 0; y < region.size[1]; ++y)
                    {
                        for (uint32_t x = 0; x < region.size[0]; ++x)
                        {
                            if (regionGrid.Test(x, y, z)
                                != fullGrid.Test(region.min[0] + x, region.min[1] + y, region.min[2] + z))
                            {
                                ++diffBits;
                            }
                        }
                    }
                }

                OccupancyBitGrid rebuilt = fullGrid;
                const OccupancyBitGrid zeroGrid(region.size[0], region.size[1], region.size[2]);
                const bool cleared = rebuilt.WriteRegion(zeroGrid, region.min);
                const bool written = rebuilt.WriteRegion(regionGrid, region.min);
                if (!cleared || !written)
                {
                    ++diffWords;
                    continue;
                }

                const std::vector<uint32_t>& fullWords = fullGrid.Words();
                const std::vector<uint32_t>& rebuiltWords = rebuilt.Words();
                for (size_t i = 0; i < fullWords.size(); ++i)
                {
                    if (fullWords[i] != rebuiltWords[i])
                    {
                        ++diffWords;
                    }
                }
            }

            result.ok = (diffBits == 0 && diffWords == 0);
            result.line = std::string(" ") + name
                + ": full=" + std::to_string(fullGrid.CountOccupied())
                + " regionVoxels=" + std::to_string(regionVoxels)
                + " diffWords=" + std::to_string(diffWords)
                + " diffBits=" + std::to_string(diffBits);
            return result;
        }
    }

    std::string RunVoxelizerSelfTest(IRhiDevice& device)
    {
        try
        {
            static GpuSurfaceVoxelizer gpu;
            if (!gpu.IsInitialized())
            {
                std::string error;
                if (!gpu.Initialize(device, error))
                {
                    return "ERR " + error;
                }
            }

            std::vector<CaseResult> results;

            {
                std::mt19937 rng(1);
                // Small triangles (edges up to ~8 voxels) scattered past the grid bounds, so the
                // case exercises clipping without making each thread loop over most of the grid.
                std::uniform_real_distribution<float> centerDist(-1.2f, 1.2f);
                std::uniform_real_distribution<float> offsetDist(-0.06f, 0.06f);
                std::vector<float> positions(6000 * 3);
                for (size_t tri = 0; tri < 2000; ++tri)
                {
                    const float center[3] = { centerDist(rng), centerDist(rng), centerDist(rng) };
                    for (size_t corner = 0; corner < 3; ++corner)
                    {
                        for (size_t axis = 0; axis < 3; ++axis)
                        {
                            positions[(tri * 3 + corner) * 3 + axis] = center[axis] + offsetDist(rng);
                        }
                    }
                }

                VoxelGridDesc desc;
                desc.origin[0] = -1.0f;
                desc.origin[1] = -1.0f;
                desc.origin[2] = -1.0f;
                desc.voxelSize = 2.0f / 128.0f;
                desc.dimX = desc.dimY = desc.dimZ = 128;

                VoxelizerMeshInput mesh;
                mesh.positions = positions.data();
                mesh.vertexCount = 6000;
                results.push_back(RunCase(gpu, "random", desc, mesh));
                results.push_back(RunRegionCase("region-random", desc, mesh));
            }

            std::vector<float> cubePositions;
            std::vector<uint32_t> cubeIndices;
            BuildCube(0.53125f, cubePositions, cubeIndices);

            VoxelizerMeshInput cubeMesh;
            cubeMesh.positions = cubePositions.data();
            cubeMesh.vertexCount = 8;
            cubeMesh.indices = cubeIndices.data();
            cubeMesh.indexCount = static_cast<uint32_t>(cubeIndices.size());

            {
                VoxelGridDesc desc;
                desc.origin[0] = -1.0f;
                desc.origin[1] = -1.0f;
                desc.origin[2] = -1.0f;
                desc.voxelSize = 1.0f / 16.0f;
                desc.dimX = desc.dimY = desc.dimZ = 32;
                results.push_back(RunCase(gpu, "cube", desc, cubeMesh));
                results.push_back(RunRegionCase("region-cube", desc, cubeMesh));
            }

            {
                VoxelGridDesc desc;
                desc.origin[0] = -1.01f;
                desc.origin[1] = -0.97f;
                desc.origin[2] = -1.003f;
                desc.voxelSize = 1.0f / 16.0f;
                desc.dimX = desc.dimY = desc.dimZ = 32;
                results.push_back(RunCase(gpu, "indexed-offgrid", desc, cubeMesh));
                results.push_back(RunRegionCase("region-offgrid", desc, cubeMesh));
            }

            bool allOk = true;
            std::string body;
            for (const CaseResult& result : results)
            {
                allOk = allOk && result.ok;
                body += result.line;
            }
            return std::string(allOk ? "OK" : "ERR") + body;
        }
        catch (const std::exception& e)
        {
            return std::string("ERR exception: ") + e.what();
        }
    }
}
