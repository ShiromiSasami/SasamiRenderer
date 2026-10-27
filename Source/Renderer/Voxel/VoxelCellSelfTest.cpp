#include "Renderer/Voxel/VoxelCellSelfTest.h"

#include "Renderer/Voxel/NibbleGrid.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/VoxelCellAttributes.h"

#include <sstream>

namespace SasamiEngine::Render::Voxel
{
    std::string RunVoxelCellSelfTest()
    {
        uint32_t checks = 0;
        std::string firstError;
        auto check = [&](bool condition, const char* description)
        {
            ++checks;
            if (!condition && firstError.empty())
            {
                firstError = description;
            }
        };

        // (a) Nibble packing. 9x3x2 = 54 cells, so cells straddle several words and the last word is partial.
        {
            NibbleGrid grid(9, 3, 2);
            check(grid.CellCount() == 54, "nibble: cell count");
            check(grid.Words().size() == 7, "nibble: word count is ceil(cells / 8)");

            uint32_t i = 0;
            for (uint32_t z = 0; z < 2; ++z)
                for (uint32_t y = 0; y < 3; ++y)
                    for (uint32_t x = 0; x < 9; ++x, ++i)
                        grid.Set(x, y, z, static_cast<uint8_t>((i * 7u) & 15u));

            bool roundTrip = true;
            i = 0;
            for (uint32_t z = 0; z < 2; ++z)
                for (uint32_t y = 0; y < 3; ++y)
                    for (uint32_t x = 0; x < 9; ++x, ++i)
                        roundTrip = roundTrip && grid.Get(x, y, z) == ((i * 7u) & 15u);
            check(roundTrip, "nibble: round trip of (i*7)&15");

            // Cells 7 and 8 sit on either side of the first word boundary.
            NibbleGrid pair(9, 3, 2);
            pair.Set(7, 0, 0, 0xF);
            pair.Set(8, 0, 0, 0x3);
            check(pair.Get(7, 0, 0) == 0xF && pair.Get(8, 0, 0) == 0x3, "nibble: neighbours across a word boundary");
            pair.Set(7, 0, 0, 0x0);
            check(pair.Get(7, 0, 0) == 0 && pair.Get(8, 0, 0) == 0x3, "nibble: clearing one cell leaves its neighbour");

            pair.Set(0, 0, 0, 0xFA);
            check(pair.Get(0, 0, 0) == 0xA && pair.Get(1, 0, 0) == 0, "nibble: Set keeps only value & 0xF");
            pair.Set(9, 0, 0, 5);
            pair.Set(0, 3, 0, 5);
            pair.Set(0, 0, 2, 5);
            check(pair.Get(9, 0, 0) == 0 && pair.Get(0, 3, 0) == 0 && pair.Get(0, 0, 2) == 0, "nibble: out of range Set is a no-op, Get reads 0");

            const std::array<uint64_t, 16> histogram = grid.Histogram();
            uint64_t sum = 0;
            for (uint64_t n : histogram)
                sum += n;
            check(sum == grid.CellCount(), "nibble: histogram sums to cell count");
            check(grid.CountNonZero() == grid.CellCount() - histogram[0], "nibble: CountNonZero matches histogram");

            grid.Clear();
            check(grid.CountNonZero() == 0, "nibble: Clear zeroes every cell");

            NibbleGrid empty(4, 0, 4);
            check(empty.CellCount() == 0 && empty.DimX() == 0 && empty.Words().empty(), "nibble: zero dim gives an empty grid");
        }

        // (b) Coverage.
        VoxelGridDesc coarseDesc;
        coarseDesc.voxelSize = 2.0f;
        coarseDesc.dimX = 2;
        coarseDesc.dimY = 2;
        coarseDesc.dimZ = 1;
        {
            const VoxelGridDesc fineDesc = MakeCoverageFineDesc(coarseDesc);
            check(fineDesc.dimX == 4 && fineDesc.dimY == 4 && fineDesc.dimZ == 2 && fineDesc.voxelSize == 1.0f, "coverage: fine desc dims and voxel size");

            OccupancyBitGrid fine(4, 4, 2);
            fine.Set(2, 0, 0);                       // coarse (1,0,0): 1
            for (uint32_t k = 0; k < 2; ++k)         // coarse (0,1,0): 8
                for (uint32_t j = 2; j < 4; ++j)
                    for (uint32_t i = 0; i < 2; ++i)
                        fine.Set(i, j, k);
            fine.Set(2, 2, 0);                       // coarse (1,1,0): 3
            fine.Set(3, 3, 1);
            fine.Set(2, 3, 0);

            NibbleGrid coverage;
            check(BuildCoverageNibbles(fine, coarseDesc, coverage), "coverage: build succeeds");
            check(coverage.DimX() == 2 && coverage.DimY() == 2 && coverage.DimZ() == 1, "coverage: output dims are the coarse dims");
            check(coverage.Get(0, 0, 0) == 0 && coverage.Get(1, 0, 0) == 1 && coverage.Get(0, 1, 0) == 8 && coverage.Get(1, 1, 0) == 3, "coverage: counts 0, 1, 8, 3");

            OccupancyBitGrid wrongFine(4, 4, 1);
            NibbleGrid untouched(3, 3, 3);
            untouched.Set(1, 1, 1, 9);
            check(!BuildCoverageNibbles(wrongFine, coarseDesc, untouched), "coverage: wrong fine dims return false");
            check(untouched.DimX() == 3 && untouched.Get(1, 1, 1) == 9, "coverage: output untouched on failure");

            VoxelGridDesc zeroDesc = coarseDesc;
            zeroDesc.dimZ = 0;
            check(!BuildCoverageNibbles(OccupancyBitGrid(), zeroDesc, untouched), "coverage: zero coarse dims return false");

            // (d) Mismatch counting against the occupancy of the same coarse grid.
            OccupancyBitGrid occupancy(2, 2, 1);
            occupancy.Set(1, 0, 0);
            occupancy.Set(0, 1, 0);
            occupancy.Set(1, 1, 0);
            check(coverage.CountNonZeroMismatches(occupancy) == 0, "mismatch: identical sets give 0");
            occupancy.Set(0, 0, 0);
            check(coverage.CountNonZeroMismatches(occupancy) == 1, "mismatch: one extra occupied cell gives 1");
            occupancy.Flip(1, 1, 0);
            check(coverage.CountNonZeroMismatches(occupancy) == 2, "mismatch: one extra and one missing cell give 2");
            check(coverage.CountNonZeroMismatches(OccupancyBitGrid(2, 2, 2)) == UINT64_MAX, "mismatch: dim mismatch gives UINT64_MAX");
        }

        // (c) Material classes.
        {
            std::vector<OccupancyBitGrid> grids(3);
            grids[1].Reset(3, 2, 2);
            grids[2].Reset(3, 2, 2);
            grids[1].Set(0, 0, 0);
            grids[1].Set(2, 1, 1);
            grids[2].Set(0, 0, 0);
            grids[2].Set(1, 0, 1);

            NibbleGrid materials;
            check(BuildMaterialClassNibbles(grids, materials), "material: build succeeds");
            check(materials.DimX() == 3 && materials.DimY() == 2 && materials.DimZ() == 2, "material: output dims are the grids' dims");
            check(materials.Get(0, 0, 0) == 2, "material: highest class wins where classes overlap");
            check(materials.Get(2, 1, 1) == 1, "material: class 1 only");
            check(materials.Get(1, 0, 1) == 2, "material: class 2 only");
            check(materials.CountNonZero() == 3, "material: no other cell is set");

            std::vector<OccupancyBitGrid> mismatched(3);
            mismatched[1].Reset(3, 2, 2);
            mismatched[2].Reset(2, 2, 2);
            check(!BuildMaterialClassNibbles(mismatched, materials), "material: mismatched dims return false");

            std::vector<OccupancyBitGrid> allEmpty(3);
            check(!BuildMaterialClassNibbles(allEmpty, materials), "material: all empty returns false");
            check(!BuildMaterialClassNibbles(std::vector<OccupancyBitGrid>(1), materials), "material: only index 0 returns false");

            std::vector<OccupancyBitGrid> tooMany(17);
            tooMany[1].Reset(2, 2, 2);
            check(!BuildMaterialClassNibbles(tooMany, materials), "material: class index 16 returns false");
        }

        // (e) Classifier.
        {
            SurfaceMaterial material;
            check(ClassifyVoxelMaterial(material) == VoxelMaterialClass::Generic, "classify: default is Generic");
            material.metallic = 0.5f;
            check(ClassifyVoxelMaterial(material) == VoxelMaterialClass::Metal, "classify: metallic 0.5 is Metal");
            material.metallic = 0.49f;
            check(ClassifyVoxelMaterial(material) == VoxelMaterialClass::Generic, "classify: metallic 0.49 is Generic");
            material.metallic = 1.0f;
            material.emissive[2] = 0.1f;
            check(ClassifyVoxelMaterial(material) == VoxelMaterialClass::Emissive, "classify: emissive beats Metal");
        }

        std::ostringstream result;
        if (!firstError.empty())
        {
            result << "ERR " << firstError;
        }
        else
        {
            result << "OK checks=" << checks;
        }
        return result.str();
    }
}
