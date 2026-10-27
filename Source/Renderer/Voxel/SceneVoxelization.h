#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/GpuSolidVoxelizer.h"
#include "Renderer/Voxel/GpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/DrawItemSnapshot.h"
#include "Renderer/Voxel/NibbleGrid.h"
#include "Renderer/Voxel/VoxelCellAttributes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    class SceneSubmitter;
}

namespace SasamiEngine::Render::Voxel
{
    enum class VoxelFillMode : uint8_t
    {
        // Voxels the surface touches (GpuSurfaceVoxelizer); closed objects stay hollow.
        Surface,
        // Surface OR interior: GpuSolidVoxelizer's crossing parity fills what closed meshes
        // enclose. Holes in the mesh leak along +X (see CountParityLeakRows).
        Solid,
    };

    // What one SceneVoxelization::Run measured. Times are wall-clock milliseconds.
    struct SceneVoxelizationStats
    {
        VoxelFillMode fillMode = VoxelFillMode::Surface;
        uint32_t drawItemsUsed = 0;
        uint32_t drawItemsSkipped = 0;
        uint64_t triangleCount = 0;
        uint32_t vertexCount = 0;
        uint64_t occupiedVoxels = 0;
        double gatherMs = 0.0;
        double gpuMs = 0.0;
        // Solid mode only: the surface and parity grids before they are OR-ed together, the
        // parity pass's time, and the rows whose parity is still odd at the grid's +X end.
        uint64_t surfaceVoxels = 0;
        uint64_t interiorVoxels = 0;
        double solidGpuMs = 0.0;
        uint64_t leakRows = 0;
        // Set only when Run was asked to diff against the CPU reference (VoxelizeSurface).
        bool cpuChecked = false;
        double cpuMs = 0.0;
        uint64_t diffBits = 0;
        // Solid mode with cpuCheck: GpuSolidVoxelizer against VoxelizeSolidParity.
        double solidCpuMs = 0.0;
        uint64_t solidDiffBits = 0;
        // Set by Update. delta: the grid was patched region by region instead of re-run.
        // fallbackReason says why a delta request ran in full ("" when it did not fall back).
        bool updated = false;
        bool delta = false;
        std::string fallbackReason;
        uint32_t movedItems = 0;
        uint32_t regions = 0;
        uint64_t regionVoxels = 0;
        // Update with check: a full GPU Voxelize of the same desc, diffed against the patched grid.
        bool fullChecked = false;
        double fullCheckMs = 0.0;
        uint64_t fullDiffBits = 0;
    };

    enum class VoxelUpdateMode : uint8_t
    {
        // Re-runs the whole voxelization with the last Run's settings (a new fit of the grid).
        Full,
        // Keeps the grid desc and re-voxelizes only the voxels around draw items whose model
        // matrix or bounds changed since the last Run/Update (old AABB union new AABB, plus one
        // voxel). Falls back to Full when the item list changed structurally, a moved item left
        // the grid, or the last result is Solid (parity fill is not local).
        Delta,
    };

    // Voxelizes the scene's opaque static geometry into one dense 1-bit occupancy grid and keeps
    // the latest result, so the debug command and the UI read the same grid. Each Run gathers the
    // world-space triangles (GatherOpaqueWorldTriangles), fits a grid to their bounds
    // (FitVoxelGridToBounds) and runs GpuSurfaceVoxelizer, and in Solid mode also
    // GpuSolidVoxelizer, OR-ing the two grids. The GPU voxelizers are created on the first Run
    // that needs them; they are DX12-only for now, so Run fails on the other backends.
    //
    // Run blocks until the GPU is idle and is meant to be called between frames on the main
    // thread, never per frame. A failed Run leaves the previous result untouched.
    // What one SceneVoxelization::RunCells measured (TODO 5d, 4-bit cells).
    struct VoxelCellStats
    {
        VoxelCellMode mode = VoxelCellMode::Coverage;
        std::array<uint64_t, 16> histogram{};
        uint64_t nonZeroCells = 0;
        uint64_t bytes = 0;
        // Cells whose nonzero-ness disagrees with the 1-bit Grid(). MaterialClass is exact
        // (expected 0); Coverage re-voxelizes at 2x and may differ at fp ties on cell faces.
        // UINT64_MAX when the result being described is Solid (Grid() then includes interiors).
        uint64_t occupancyMismatches = 0;
        uint32_t passes = 0;             // GPU voxelizations run (Coverage 1, MaterialClass one per used class)
        uint32_t drawItemsUsed = 0;
        double gatherMs = 0.0;
        double gpuMs = 0.0;
        double buildMs = 0.0;            // CPU nibble packing
        // MaterialClass: opaque draw items per class (index = VoxelMaterialClass value).
        std::array<uint32_t, VOXEL_MATERIAL_CLASS_COUNT> itemsPerClass{};
    };

    class SceneVoxelization
    {
    public:
        // maxDim is the voxel count along the scene's longest axis (2..512).
        bool Run(IRhiDevice& device,
                 const SceneSubmitter& submitter,
                 uint32_t maxDim,
                 VoxelFillMode fillMode,
                 bool cpuCheck,
                 std::string& outError);

        // Brings the latest result up to date after objects moved (needs a previous Run).
        // check additionally runs a full GPU voxelization of the same desc and diffs it.
        bool Update(IRhiDevice& device,
                    const SceneSubmitter& submitter,
                    VoxelUpdateMode mode,
                    bool check,
                    std::string& outError);

        bool HasResult() const { return m_generation != 0; }
        // Changes on every successful Run and is unique across instances (a process-wide
        // counter), so a cache keyed by it never matches a result from a replaced instance.
        // 0 means no result yet.
        uint64_t Generation() const { return m_generation; }
        const VoxelGridDesc& Desc() const { return m_desc; }
        const OccupancyBitGrid& Grid() const { return m_grid; }
        const SceneVoxelizationStats& Stats() const { return m_stats; }

        // Builds 4-bit cells for the latest result's grid desc (needs a previous Run; a Solid
        // result's interior is not included, the cells describe surfaces). Coverage voxelizes
        // the scene again at twice the resolution; MaterialClass voxelizes each material class
        // separately. Coverage is limited to coarse dims <= 256 (the 2x grid would pass 512).
        // Uses the current draw items, so call it right after Run/Update to get
        // cells consistent with Grid(). A failed call leaves the previous cells untouched.
        bool RunCells(IRhiDevice& device,
                      const SceneSubmitter& submitter,
                      VoxelCellMode mode,
                      std::string& outError);
        // Generation() of the result the cells were built against; 0 = no cells yet.
        uint64_t CellsGeneration() const { return m_cellsGeneration; }
        const NibbleGrid& Cells() const { return m_cells; }
        const VoxelCellStats& CellStats() const { return m_cellStats; }

    private:
        GpuSurfaceVoxelizer m_gpuVoxelizer;
        GpuSolidVoxelizer m_gpuSolidVoxelizer;
        VoxelGridDesc m_desc{};
        OccupancyBitGrid m_grid;
        SceneVoxelizationStats m_stats{};
        uint64_t m_generation = 0;
        // Settings of the last successful Run, reused by Update.
        uint32_t m_lastMaxDim = 0;
        bool m_lastCpuCheck = false;
        // Draw items as of the last successful Run/Update, for Delta.
        DrawItemSnapshot m_snapshot;
        NibbleGrid m_cells;
        VoxelCellStats m_cellStats{};
        uint64_t m_cellsGeneration = 0;
    };
}

namespace SasamiEngine { using namespace Render; }
