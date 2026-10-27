#pragma once

#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class NibbleGrid;
    class OccupancyBitGrid;

    // What the 4 bits of a cell mean. Both modes keep 0 = empty, so the nonzero set of either
    // equals the 1-bit surface occupancy of the same grid desc.
    enum class VoxelCellMode : uint8_t
    {
        // Sub-voxel coverage: how many of the cell's 2x2x2 sub-voxels the surface touches (0..8).
        // The fractional solid weight of variational solid-fluid coupling
        // (Batty, Bertails & Bridson 2007), quantized to eighths.
        Coverage,
        // Material class 1..15 (VoxelMaterialClass) of the surfaces in the cell; where several
        // classes meet, the highest one wins. The per-material key the solid-side heat transfer
        // (TODO 5b (ii)) needs.
        MaterialClass,
    };

    // Material classes. Values are the cell nibbles; ordered so that "highest wins" keeps the
    // class that matters most for the neighbouring fluid (metal conducts, emissive heats).
    enum class VoxelMaterialClass : uint8_t
    {
        Empty    = 0,
        Generic  = 1,    // dielectric opaque surface (stone, wood, plaster...)
        Metal    = 2,    // metallic >= 0.5
        Emissive = 3,    // any emissive channel > 0
    };
    constexpr uint32_t VOXEL_MATERIAL_CLASS_COUNT = 4;   // including Empty

    // Default classifier for one draw item's material. Emissive beats Metal beats Generic.
    VoxelMaterialClass ClassifyVoxelMaterial(const SurfaceMaterial& material);

    // Grid desc at twice the resolution: same origin, voxelSize / 2, dims * 2. Fine voxel
    // (2x+i, 2y+j, 2z+k), i,j,k in {0,1}, is a sub-voxel of coarse voxel (x, y, z).
    VoxelGridDesc MakeCoverageFineDesc(const VoxelGridDesc& coarse);

    // out (resized to coarse's dims) = popcount of the 8 fine sub-voxels of each coarse cell.
    // fine must have exactly 2x coarse's dims; returns false otherwise.
    bool BuildCoverageNibbles(const OccupancyBitGrid& fine, const VoxelGridDesc& coarse, NibbleGrid& out);

    // classGrids[c] is the surface occupancy of the triangles of class c (index 0 is ignored and
    // may be empty). out (resized to the grids' dims) = the highest c whose grid is set, else 0.
    // Every non-empty grid must share the same dims; returns false otherwise or when all are empty.
    bool BuildMaterialClassNibbles(const std::vector<OccupancyBitGrid>& classGrids, NibbleGrid& out);
}

namespace SasamiEngine { using namespace Render; }
