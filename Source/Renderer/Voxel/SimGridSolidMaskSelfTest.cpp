#include "Renderer/Voxel/SimGridSolidMaskSelfTest.h"

#include "Renderer/Voxel/SimGridSolidMask.h"

#include <bitset>
#include <random>
#include <sstream>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        VoxelGridDesc MakeVoxelDesc(float ox, float oy, float oz, float voxelSize, uint32_t dx, uint32_t dy, uint32_t dz)
        {
            VoxelGridDesc desc;
            desc.origin[0] = ox;
            desc.origin[1] = oy;
            desc.origin[2] = oz;
            desc.voxelSize = voxelSize;
            desc.dimX = dx;
            desc.dimY = dy;
            desc.dimZ = dz;
            return desc;
        }

        SimGridDesc MakeSimDesc(float ox, float oy, float oz, float cellSize, uint32_t cx, uint32_t cy, uint32_t cz)
        {
            SimGridDesc desc;
            desc.origin[0] = ox;
            desc.origin[1] = oy;
            desc.origin[2] = oz;
            desc.cellSize = cellSize;
            desc.countX = cx;
            desc.countY = cy;
            desc.countZ = cz;
            return desc;
        }

        OccupancyBitGrid MakeRandomGrid(uint32_t dx, uint32_t dy, uint32_t dz, uint32_t percent, uint32_t seed)
        {
            OccupancyBitGrid grid(dx, dy, dz);
            std::mt19937 rng(seed);
            std::uniform_int_distribution<uint32_t> pick(0, 99);
            for (uint32_t z = 0; z < dz; ++z)
                for (uint32_t y = 0; y < dy; ++y)
                    for (uint32_t x = 0; x < dx; ++x)
                        if (pick(rng) < percent)
                            grid.Set(x, y, z);
            return grid;
        }

        bool WordsBit(const std::vector<uint32_t>& words, uint64_t idx)
        {
            const size_t w = static_cast<size_t>(idx >> 5);
            return w < words.size() && ((words[w] >> (idx & 31)) & 1u) != 0;
        }

        uint64_t PopCount(const std::vector<uint32_t>& words)
        {
            uint64_t n = 0;
            for (uint32_t w : words)
                n += std::bitset<32>(w).count();
            return n;
        }

        // Open-interval overlap of every voxel against every cell, in double, with no index-range shortcut.
        std::vector<uint32_t> BruteForceMask(const VoxelGridDesc& vd, const OccupancyBitGrid& occ, const SimGridDesc& sim)
        {
            const uint64_t cellCount = static_cast<uint64_t>(sim.countX) * sim.countY * sim.countZ;
            std::vector<uint32_t> words(static_cast<size_t>((cellCount + 31) / 32), 0u);
            const double vs = vd.voxelSize;
            const double cs = sim.cellSize;
            for (uint32_t k = 0; k < sim.countZ; ++k)
                for (uint32_t j = 0; j < sim.countY; ++j)
                    for (uint32_t i = 0; i < sim.countX; ++i)
                    {
                        const double cMin[3] = { sim.origin[0] + i * cs - 0.5 * cs,
                                                 sim.origin[1] + j * cs - 0.5 * cs,
                                                 sim.origin[2] + k * cs - 0.5 * cs };
                        bool solid = false;
                        for (uint32_t z = 0; z < vd.dimZ && !solid; ++z)
                            for (uint32_t y = 0; y < vd.dimY && !solid; ++y)
                                for (uint32_t x = 0; x < vd.dimX && !solid; ++x)
                                {
                                    if (!occ.Test(x, y, z))
                                        continue;
                                    const double vMin[3] = { vd.origin[0] + x * vs, vd.origin[1] + y * vs, vd.origin[2] + z * vs };
                                    solid = vMin[0] < cMin[0] + cs && vMin[0] + vs > cMin[0] &&
                                            vMin[1] < cMin[1] + cs && vMin[1] + vs > cMin[1] &&
                                            vMin[2] < cMin[2] + cs && vMin[2] + vs > cMin[2];
                                }
                        if (solid)
                        {
                            const uint64_t idx = i + static_cast<uint64_t>(j) * sim.countX +
                                                 static_cast<uint64_t>(k) * sim.countX * sim.countY;
                            words[static_cast<size_t>(idx >> 5)] |= 1u << (idx & 31);
                        }
                    }
            return words;
        }

        // Expected mask for sim == voxel grid when the boxes coincide: the occupancy bits themselves.
        std::vector<uint32_t> MaskFromOccupancy(const OccupancyBitGrid& occ)
        {
            std::vector<uint32_t> words((static_cast<size_t>(occ.VoxelCount()) + 31) / 32, 0u);
            for (uint32_t z = 0; z < occ.DimZ(); ++z)
                for (uint32_t y = 0; y < occ.DimY(); ++y)
                    for (uint32_t x = 0; x < occ.DimX(); ++x)
                        if (occ.Test(x, y, z))
                        {
                            const uint64_t idx = x + static_cast<uint64_t>(y) * occ.DimX() +
                                                 static_cast<uint64_t>(z) * occ.DimX() * occ.DimY();
                            words[static_cast<size_t>(idx >> 5)] |= 1u << (idx & 31);
                        }
            return words;
        }

        // Runs the builder and compares with `expected`; on a mismatch writes the case name and the
        // first differing cell to `report`, otherwise appends "name:solid=N".
        bool CheckCase(const char* name, const VoxelGridDesc& vd, const OccupancyBitGrid& occ, const SimGridDesc& sim,
                       const std::vector<uint32_t>& expected, std::string& report)
        {
            std::vector<uint32_t> got;
            const uint64_t solid = BuildSimGridSolidMask(vd, occ, sim, got);
            std::ostringstream line;
            if (got.size() != expected.size())
            {
                line << name << ": word count " << got.size() << " != expected " << expected.size();
                report += line.str();
                return false;
            }
            const uint64_t cellCount = static_cast<uint64_t>(sim.countX) * sim.countY * sim.countZ;
            for (uint64_t idx = 0; idx < cellCount; ++idx)
            {
                if (WordsBit(got, idx) == WordsBit(expected, idx))
                    continue;
                const uint64_t layer = static_cast<uint64_t>(sim.countX) * sim.countY;
                line << name << ": first mismatch at cell (" << idx % sim.countX << "," << (idx / sim.countX) % sim.countY
                     << "," << idx / layer << ") got=" << WordsBit(got, idx) << " expected=" << WordsBit(expected, idx);
                report += line.str();
                return false;
            }
            if (solid != PopCount(expected))
            {
                line << name << ": returned solid=" << solid << " != expected " << PopCount(expected);
                report += line.str();
                return false;
            }
            line << name << ":solid=" << solid;
            report += line.str();
            return true;
        }

        bool CheckAligned(std::string& report)
        {
            const OccupancyBitGrid occ = MakeRandomGrid(23, 17, 29, 20, 12345u);
            const VoxelGridDesc vd = MakeVoxelDesc(1.5f, -2.0f, 0.25f, 0.5f, 23, 17, 29);
            const SimGridDesc sim = MakeSimDesc(1.75f, -1.75f, 0.5f, 0.5f, 23, 17, 29);
            return CheckCase("aligned", vd, occ, sim, MaskFromOccupancy(occ), report);
        }

        bool CheckCoarser(std::string& report)
        {
            const OccupancyBitGrid occ = MakeRandomGrid(23, 17, 29, 20, 12345u);
            const VoxelGridDesc vd = MakeVoxelDesc(1.5f, -2.0f, 0.25f, 0.5f, 23, 17, 29);
            const SimGridDesc sim = MakeSimDesc(1.5f, -2.0f, 0.25f, 0.5f * 1.1f, 12, 9, 15);
            return CheckCase("coarser", vd, occ, sim, BruteForceMask(vd, occ, sim), report);
        }

        bool CheckFiner(std::string& report)
        {
            const OccupancyBitGrid occ = MakeRandomGrid(23, 17, 29, 20, 12345u);
            const VoxelGridDesc vd = MakeVoxelDesc(1.5f, -2.0f, 0.25f, 0.5f, 23, 17, 29);
            const SimGridDesc sim = MakeSimDesc(1.5f + 0.07f, -2.0f - 0.11f, 0.25f + 0.13f, 0.5f * 0.3f, 18, 14, 20);
            return CheckCase("finer", vd, occ, sim, BruteForceMask(vd, occ, sim), report);
        }

        bool CheckDisjoint(std::string& report)
        {
            const OccupancyBitGrid occ = MakeRandomGrid(23, 17, 29, 20, 12345u);
            const VoxelGridDesc vd = MakeVoxelDesc(1.5f, -2.0f, 0.25f, 0.5f, 23, 17, 29);
            const SimGridDesc sim = MakeSimDesc(100.0f, 100.0f, 100.0f, 0.5f, 8, 8, 8);
            return CheckCase("disjoint", vd, occ, sim, std::vector<uint32_t>(16, 0u), report);
        }

        bool CheckSingleVoxel(std::string& report)
        {
            OccupancyBitGrid occ(6, 6, 6);
            occ.Set(2, 3, 4);
            const VoxelGridDesc vd = MakeVoxelDesc(0.0f, 0.0f, 0.0f, 1.0f, 6, 6, 6);
            const SimGridDesc sim = MakeSimDesc(0.125f, 0.125f, 0.125f, 0.25f, 24, 24, 24);
            std::vector<uint32_t> expected((24u * 24u * 24u + 31) / 32, 0u);
            for (uint32_t z = 16; z <= 19; ++z)
                for (uint32_t y = 12; y <= 15; ++y)
                    for (uint32_t x = 8; x <= 11; ++x)
                    {
                        const uint64_t idx = x + y * 24u + z * 24u * 24u;
                        expected[static_cast<size_t>(idx >> 5)] |= 1u << (idx & 31);
                    }
            return CheckCase("single", vd, occ, sim, expected, report);
        }
    }

    std::string RunSimGridSolidMaskSelfTest()
    {
        bool (*const checks[])(std::string&) = { CheckAligned, CheckCoarser, CheckFiner, CheckDisjoint, CheckSingleVoxel };
        bool allOk = true;
        std::string body;
        for (auto check : checks)
        {
            std::string line;
            const bool ok = check(line);
            allOk = allOk && ok;
            body += (ok ? " " : " [FAIL] ") + line;
        }
        return std::string(allOk ? "OK" : "ERR") + body;
    }
}
