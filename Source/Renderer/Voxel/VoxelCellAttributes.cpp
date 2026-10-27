#include "Renderer/Voxel/VoxelCellAttributes.h"

#include "Renderer/Voxel/NibbleGrid.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"

namespace SasamiEngine::Render::Voxel
{
    VoxelMaterialClass ClassifyVoxelMaterial(const SurfaceMaterial& material)
    {
        if (material.emissive[0] > 0.0f || material.emissive[1] > 0.0f || material.emissive[2] > 0.0f)
        {
            return VoxelMaterialClass::Emissive;
        }
        if (material.metallic >= 0.5f)
        {
            return VoxelMaterialClass::Metal;
        }
        return VoxelMaterialClass::Generic;
    }

    VoxelGridDesc MakeCoverageFineDesc(const VoxelGridDesc& coarse)
    {
        VoxelGridDesc fine = coarse;
        fine.voxelSize = coarse.voxelSize * 0.5f;
        fine.dimX = coarse.dimX * 2u;
        fine.dimY = coarse.dimY * 2u;
        fine.dimZ = coarse.dimZ * 2u;
        return fine;
    }

    bool BuildCoverageNibbles(const OccupancyBitGrid& fine, const VoxelGridDesc& coarse, NibbleGrid& out)
    {
        if (coarse.dimX == 0 || coarse.dimY == 0 || coarse.dimZ == 0)
        {
            return false;
        }
        if (fine.DimX() != coarse.dimX * 2u || fine.DimY() != coarse.dimY * 2u || fine.DimZ() != coarse.dimZ * 2u)
        {
            return false;
        }

        out.Reset(coarse.dimX, coarse.dimY, coarse.dimZ);
        for (uint32_t z = 0; z < coarse.dimZ; ++z)
        {
            for (uint32_t y = 0; y < coarse.dimY; ++y)
            {
                for (uint32_t x = 0; x < coarse.dimX; ++x)
                {
                    uint8_t count = 0;
                    for (uint32_t k = 0; k < 2; ++k)
                    {
                        for (uint32_t j = 0; j < 2; ++j)
                        {
                            for (uint32_t i = 0; i < 2; ++i)
                            {
                                if (fine.Test(2u * x + i, 2u * y + j, 2u * z + k))
                                {
                                    ++count;
                                }
                            }
                        }
                    }
                    out.Set(x, y, z, count);
                }
            }
        }
        return true;
    }

    bool BuildMaterialClassNibbles(const std::vector<OccupancyBitGrid>& classGrids, NibbleGrid& out)
    {
        // A nibble holds 0..15, so class indices from 16 up cannot be represented.
        if (classGrids.size() <= 1 || classGrids.size() > 16)
        {
            return false;
        }

        const OccupancyBitGrid* reference = nullptr;
        for (size_t c = 1; c < classGrids.size(); ++c)
        {
            const OccupancyBitGrid& grid = classGrids[c];
            if (grid.VoxelCount() == 0)
            {
                continue;
            }
            if (reference == nullptr)
            {
                reference = &grid;
            }
            else if (grid.DimX() != reference->DimX() || grid.DimY() != reference->DimY() || grid.DimZ() != reference->DimZ())
            {
                return false;
            }
        }
        if (reference == nullptr)
        {
            return false;
        }

        out.Reset(reference->DimX(), reference->DimY(), reference->DimZ());
        for (size_t c = 1; c < classGrids.size(); ++c)
        {
            const OccupancyBitGrid& grid = classGrids[c];
            if (grid.VoxelCount() == 0)
            {
                continue;
            }
            for (uint32_t z = 0; z < grid.DimZ(); ++z)
            {
                for (uint32_t y = 0; y < grid.DimY(); ++y)
                {
                    for (uint32_t x = 0; x < grid.DimX(); ++x)
                    {
                        if (grid.Test(x, y, z))
                        {
                            out.Set(x, y, z, static_cast<uint8_t>(c));
                        }
                    }
                }
            }
        }
        return true;
    }
}
