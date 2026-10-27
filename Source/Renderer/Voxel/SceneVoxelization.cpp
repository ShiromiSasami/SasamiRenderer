#include "Renderer/Voxel/SceneVoxelization.h"

#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/Voxel/CpuSolidVoxelizer.h"
#include "Renderer/Voxel/ParityLeakCount.h"
#include "Renderer/Voxel/VoxelGridFit.h"
#include "Renderer/Voxel/WorldTriangleGather.h"
#include "Foundation/Tools/ScopedPerfTimer.h"

#include <bitset>
#include <cstddef>
#include <chrono>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        using Foundation::MillisecondsSince;

        // Run is main-thread only, so a plain counter is enough.
        uint64_t g_nextGeneration = 0;

        bool CountDiffBits(const OccupancyBitGrid& a, const OccupancyBitGrid& b, uint64_t& outDiffBits, std::string& outError)
        {
            const std::vector<uint32_t>& aWords = a.Words();
            const std::vector<uint32_t>& bWords = b.Words();
            if (aWords.size() != bWords.size()) {
                outError = "cpu/gpu grid size mismatch";
                return false;
            }
            outDiffBits = 0;
            for (size_t i = 0; i < aWords.size(); ++i) {
                outDiffBits += std::bitset<32>(aWords[i] ^ bWords[i]).count();
            }
            return true;
        }
    }

    bool SceneVoxelization::Run(IRhiDevice& device,
                                const SceneSubmitter& submitter,
                                uint32_t maxDim,
                                VoxelFillMode fillMode,
                                bool cpuCheck,
                                std::string& outError)
    {
        if (maxDim < 2 || maxDim > 512) {
            outError = "maxDim out of range";
            return false;
        }

        auto stepStart = std::chrono::steady_clock::now();
        const WorldTriangleSoup soup = GatherOpaqueWorldTriangles(submitter.GetDrawItems(), submitter.GetMeshes());
        const double gatherMs = MillisecondsSince(stepStart);
        if (!soup.hasBounds) {
            outError = "no opaque triangles in scene";
            return false;
        }

        const VoxelGridDesc desc = FitVoxelGridToBounds(soup.boundsMin, soup.boundsMax, maxDim);
        if (desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0) {
            outError = "could not fit grid";
            return false;
        }

        VoxelizerMeshInput mesh;
        mesh.positions = soup.positions.data();
        mesh.vertexCount = static_cast<uint32_t>(soup.positions.size() / 3);
        mesh.positionStrideInFloats = 3;
        mesh.indices = soup.indices.data();
        mesh.indexCount = static_cast<uint32_t>(soup.indices.size());

        if (!m_gpuVoxelizer.IsInitialized() && !m_gpuVoxelizer.Initialize(device, outError)) {
            return false;
        }

        OccupancyBitGrid gpuGrid;
        stepStart = std::chrono::steady_clock::now();
        if (!m_gpuVoxelizer.Voxelize(desc, mesh, gpuGrid, outError)) {
            return false;
        }
        const double gpuMs = MillisecondsSince(stepStart);

        double cpuMs = 0.0;
        uint64_t diffBits = 0;
        if (cpuCheck) {
            OccupancyBitGrid cpuGrid(desc.dimX, desc.dimY, desc.dimZ);
            stepStart = std::chrono::steady_clock::now();
            VoxelizeSurface(desc, mesh, cpuGrid);
            cpuMs = MillisecondsSince(stepStart);
            if (!CountDiffBits(cpuGrid, gpuGrid, diffBits, outError)) {
                return false;
            }
        }

        SceneVoxelizationStats stats;
        stats.fillMode = fillMode;
        if (fillMode == VoxelFillMode::Solid) {
            if (!m_gpuSolidVoxelizer.IsInitialized() && !m_gpuSolidVoxelizer.Initialize(device, outError)) {
                return false;
            }

            OccupancyBitGrid interiorGrid;
            stepStart = std::chrono::steady_clock::now();
            if (!m_gpuSolidVoxelizer.Voxelize(desc, mesh, interiorGrid, outError)) {
                return false;
            }
            stats.solidGpuMs = MillisecondsSince(stepStart);

            if (cpuCheck) {
                OccupancyBitGrid cpuInterior(desc.dimX, desc.dimY, desc.dimZ);
                stepStart = std::chrono::steady_clock::now();
                VoxelizeSolidParity(desc, mesh, cpuInterior);
                stats.solidCpuMs = MillisecondsSince(stepStart);
                if (!CountDiffBits(cpuInterior, interiorGrid, stats.solidDiffBits, outError)) {
                    return false;
                }
            }

            stats.surfaceVoxels = gpuGrid.CountOccupied();
            stats.interiorVoxels = interiorGrid.CountOccupied();
            stats.leakRows = CountParityLeakRows(interiorGrid);
            if (!gpuGrid.OrWith(interiorGrid)) {
                outError = "surface/interior grid size mismatch";
                return false;
            }
        }
        stats.drawItemsUsed = soup.drawItemsUsed;
        stats.drawItemsSkipped = soup.drawItemsSkipped;
        stats.triangleCount = soup.indices.size() / 3;
        stats.vertexCount = mesh.vertexCount;
        stats.occupiedVoxels = gpuGrid.CountOccupied();
        stats.gatherMs = gatherMs;
        stats.gpuMs = gpuMs;
        stats.cpuChecked = cpuCheck;
        stats.cpuMs = cpuMs;
        stats.diffBits = diffBits;

        m_grid = std::move(gpuGrid);
        m_desc = desc;
        m_stats = stats;
        m_generation = ++g_nextGeneration;
        m_lastMaxDim = maxDim;
        m_lastCpuCheck = cpuCheck;
        m_snapshot.Capture(submitter.GetDrawItems(), submitter.GetMeshes());
        return true;
    }

    bool SceneVoxelization::Update(IRhiDevice& device,
                                   const SceneSubmitter& submitter,
                                   VoxelUpdateMode mode,
                                   bool check,
                                   std::string& outError)
    {
        if (!HasResult())
        {
            outError = "no previous voxel.scene result";
            return false;
        }

        const std::vector<SceneSubmitter::DrawItem>& drawItems = submitter.GetDrawItems();
        const std::vector<Mesh>& meshes = submitter.GetMeshes();

        auto runFull = [&](const char* reason) -> bool
        {
            if (!Run(device, submitter, m_lastMaxDim, m_stats.fillMode, m_lastCpuCheck, outError))
            {
                return false;
            }
            m_stats.updated = true;
            m_stats.delta = false;
            m_stats.fallbackReason = reason;
            return true;
        };

        if (mode == VoxelUpdateMode::Full)
        {
            return runFull("");
        }
        if (m_stats.fillMode == VoxelFillMode::Solid)
        {
            return runFull("solid");
        }

        const DrawItemDiff diff = m_snapshot.Compare(drawItems, meshes);
        if (diff.structural)
        {
            return runFull("structural");
        }

        if (diff.changedItems.empty())
        {
            m_stats.updated = true;
            m_stats.delta = true;
            m_stats.fallbackReason.clear();
            m_stats.movedItems = 0;
            m_stats.regions = 0;
            m_stats.regionVoxels = 0;
            m_stats.fullChecked = false;
            m_stats.fullCheckMs = 0.0;
            m_stats.fullDiffBits = 0;
            return true;
        }

        // A delta keeps m_desc, so an item whose new bounds leave the grid needs a refit.
        float gridMax[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            const uint32_t dim = axis == 0 ? m_desc.dimX : (axis == 1 ? m_desc.dimY : m_desc.dimZ);
            gridMax[axis] = m_desc.origin[axis] + static_cast<float>(dim) * m_desc.voxelSize;
        }
        for (uint32_t itemIndex : diff.changedItems)
        {
            const SceneSubmitter::DrawItem& item = drawItems[itemIndex];
            for (int axis = 0; axis < 3; ++axis)
            {
                if (item.worldBoundsMin[axis] < m_desc.origin[axis] || item.worldBoundsMax[axis] > gridMax[axis])
                {
                    return runFull("left grid");
                }
            }
        }

        if (!m_gpuVoxelizer.IsInitialized() && !m_gpuVoxelizer.Initialize(device, outError))
        {
            return false;
        }

        std::vector<VoxelRegion> regions;
        for (size_t i = 0; i < diff.changedItems.size(); ++i)
        {
            VoxelRegion region;
            if (VoxelRegionFromWorldBounds(m_desc, &diff.dirtyBounds[i * 6], &diff.dirtyBounds[i * 6 + 3], 1, region))
            {
                regions.push_back(region);
            }
        }

        bool merged = true;
        while (merged)
        {
            merged = false;
            for (size_t a = 0; a < regions.size() && !merged; ++a)
            {
                for (size_t b = a + 1; b < regions.size(); ++b)
                {
                    if (VoxelRegionsOverlap(regions[a], regions[b]))
                    {
                        regions[a] = UnionVoxelRegions(regions[a], regions[b]);
                        regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(b));
                        merged = true;
                        break;
                    }
                }
            }
        }

        SceneVoxelizationStats stats = m_stats;
        stats.updated = true;
        stats.delta = true;
        stats.fallbackReason.clear();
        stats.movedItems = static_cast<uint32_t>(diff.changedItems.size());
        stats.regions = 0;
        stats.regionVoxels = 0;
        stats.gatherMs = 0.0;
        stats.gpuMs = 0.0;
        stats.cpuChecked = false;
        stats.fullChecked = false;
        stats.fullCheckMs = 0.0;
        stats.fullDiffBits = 0;

        // From here on m_grid may be patched only in part if a step fails; dropping the snapshot
        // makes the next Update fall back to a full Run instead of trusting that grid.
        for (const VoxelRegion& region : regions)
        {
            float boxMin[3];
            float boxMax[3];
            VoxelRegionWorldBounds(m_desc, region, boxMin, boxMax);
            for (int axis = 0; axis < 3; ++axis)
            {
                boxMin[axis] -= m_desc.voxelSize;
                boxMax[axis] += m_desc.voxelSize;
            }

            // A voxel in the region can be touched by an unmoved triangle whose AABB overlaps it,
            // so every opaque item overlapping the (one voxel widened) region is re-voxelized into
            // it, not only the moved one. The region is overwritten, bits set and cleared alike.
            auto stepStart = std::chrono::steady_clock::now();
            std::vector<uint32_t> itemIndices;
            for (size_t i = 0; i < drawItems.size(); ++i)
            {
                const SceneSubmitter::DrawItem& item = drawItems[i];
                if (item.transparent || !item.hasWorldBounds || item.meshIndex >= meshes.size())
                {
                    continue;
                }
                bool overlaps = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (item.worldBoundsMax[axis] < boxMin[axis] || item.worldBoundsMin[axis] > boxMax[axis])
                    {
                        overlaps = false;
                        break;
                    }
                }
                if (overlaps)
                {
                    itemIndices.push_back(static_cast<uint32_t>(i));
                }
            }
            const WorldTriangleSoup soup = GatherOpaqueWorldTriangles(drawItems, meshes, itemIndices);
            stats.gatherMs += MillisecondsSince(stepStart);

            OccupancyBitGrid regionGrid;
            if (soup.indices.empty())
            {
                regionGrid.Reset(region.size[0], region.size[1], region.size[2]);
            }
            else
            {
                VoxelizerMeshInput mesh;
                mesh.positions = soup.positions.data();
                mesh.vertexCount = static_cast<uint32_t>(soup.positions.size() / 3);
                mesh.positionStrideInFloats = 3;
                mesh.indices = soup.indices.data();
                mesh.indexCount = static_cast<uint32_t>(soup.indices.size());

                stepStart = std::chrono::steady_clock::now();
                if (!m_gpuVoxelizer.VoxelizeRegion(m_desc, region, mesh, regionGrid, outError))
                {
                    m_snapshot.Clear();
                    return false;
                }
                stats.gpuMs += MillisecondsSince(stepStart);
            }

            if (!m_grid.WriteRegion(regionGrid, region.min))
            {
                m_snapshot.Clear();
                outError = "region write out of grid";
                return false;
            }
            ++stats.regions;
            stats.regionVoxels += region.VoxelCount();
        }

        stats.occupiedVoxels = m_grid.CountOccupied();
        stats.surfaceVoxels = stats.occupiedVoxels;
        m_stats = stats;
        m_generation = ++g_nextGeneration;
        m_snapshot.Capture(drawItems, meshes);

        if (check)
        {
            const WorldTriangleSoup fullSoup = GatherOpaqueWorldTriangles(drawItems, meshes);
            VoxelizerMeshInput mesh;
            mesh.positions = fullSoup.positions.data();
            mesh.vertexCount = static_cast<uint32_t>(fullSoup.positions.size() / 3);
            mesh.positionStrideInFloats = 3;
            mesh.indices = fullSoup.indices.data();
            mesh.indexCount = static_cast<uint32_t>(fullSoup.indices.size());

            const auto stepStart = std::chrono::steady_clock::now();
            OccupancyBitGrid fullGrid;
            if (!m_gpuVoxelizer.Voxelize(m_desc, mesh, fullGrid, outError))
            {
                return false;
            }
            m_stats.fullCheckMs = MillisecondsSince(stepStart);
            if (!CountDiffBits(m_grid, fullGrid, m_stats.fullDiffBits, outError))
            {
                return false;
            }
            m_stats.fullChecked = true;
        }
        return true;
    }

    bool SceneVoxelization::RunCells(IRhiDevice& device,
                                     const SceneSubmitter& submitter,
                                     VoxelCellMode mode,
                                     std::string& outError)
    {
        if (!HasResult()) {
            outError = "no voxel.scene result";
            return false;
        }

        const std::vector<SceneSubmitter::DrawItem>& drawItems = submitter.GetDrawItems();
        const std::vector<Mesh>& meshes = submitter.GetMeshes();

        if (!m_gpuVoxelizer.IsInitialized() && !m_gpuVoxelizer.Initialize(device, outError)) {
            return false;
        }

        VoxelCellStats stats;
        stats.mode = mode;
        NibbleGrid cells;
        double buildMs = 0.0;

        auto buildMesh = [](const WorldTriangleSoup& soup) {
            VoxelizerMeshInput mesh;
            mesh.positions = soup.positions.data();
            mesh.vertexCount = static_cast<uint32_t>(soup.positions.size() / 3);
            mesh.positionStrideInFloats = 3;
            mesh.indices = soup.indices.data();
            mesh.indexCount = static_cast<uint32_t>(soup.indices.size());
            return mesh;
        };

        if (mode == VoxelCellMode::Coverage) {
            if (m_desc.dimX > 256 || m_desc.dimY > 256 || m_desc.dimZ > 256) {
                outError = "coverage needs coarse dims <= 256";
                return false;
            }

            auto stepStart = std::chrono::steady_clock::now();
            const WorldTriangleSoup soup = GatherOpaqueWorldTriangles(drawItems, meshes);
            stats.gatherMs = MillisecondsSince(stepStart);
            if (!soup.hasBounds) {
                outError = "no opaque triangles in scene";
                return false;
            }

            const VoxelGridDesc fineDesc = MakeCoverageFineDesc(m_desc);
            const VoxelizerMeshInput mesh = buildMesh(soup);
            OccupancyBitGrid fineGrid;
            stepStart = std::chrono::steady_clock::now();
            if (!m_gpuVoxelizer.Voxelize(fineDesc, mesh, fineGrid, outError)) {
                return false;
            }
            stats.gpuMs = MillisecondsSince(stepStart);
            stats.passes = 1;
            stats.drawItemsUsed = soup.drawItemsUsed;

            stepStart = std::chrono::steady_clock::now();
            if (!BuildCoverageNibbles(fineGrid, m_desc, cells)) {
                outError = "coverage grid size mismatch";
                return false;
            }
            buildMs = MillisecondsSince(stepStart);
        } else {
            std::vector<std::vector<uint32_t>> classItems(VOXEL_MATERIAL_CLASS_COUNT);
            for (size_t i = 0; i < drawItems.size(); ++i) {
                const SceneSubmitter::DrawItem& item = drawItems[i];
                if (item.transparent) {
                    continue;
                }
                const uint32_t classIndex = static_cast<uint32_t>(ClassifyVoxelMaterial(item.material));
                if (classIndex == 0 || classIndex >= VOXEL_MATERIAL_CLASS_COUNT) {
                    continue;
                }
                classItems[classIndex].push_back(static_cast<uint32_t>(i));
                ++stats.itemsPerClass[classIndex];
            }

            std::vector<OccupancyBitGrid> classGrids(VOXEL_MATERIAL_CLASS_COUNT);
            for (uint32_t c = 1; c < VOXEL_MATERIAL_CLASS_COUNT; ++c) {
                if (classItems[c].empty()) {
                    continue;
                }
                auto stepStart = std::chrono::steady_clock::now();
                const WorldTriangleSoup soup = GatherOpaqueWorldTriangles(drawItems, meshes, classItems[c]);
                stats.gatherMs += MillisecondsSince(stepStart);
                if (soup.indices.empty()) {
                    continue;
                }

                const VoxelizerMeshInput mesh = buildMesh(soup);
                stepStart = std::chrono::steady_clock::now();
                if (!m_gpuVoxelizer.Voxelize(m_desc, mesh, classGrids[c], outError)) {
                    return false;
                }
                stats.gpuMs += MillisecondsSince(stepStart);
                ++stats.passes;
                stats.drawItemsUsed += soup.drawItemsUsed;
            }

            const auto stepStart = std::chrono::steady_clock::now();
            if (!BuildMaterialClassNibbles(classGrids, cells)) {
                outError = "no opaque triangles in scene";
                return false;
            }
            buildMs = MillisecondsSince(stepStart);
        }

        stats.buildMs = buildMs;
        stats.histogram = cells.Histogram();
        stats.nonZeroCells = cells.CountNonZero();
        stats.bytes = cells.SizeInBytes();
        stats.occupancyMismatches = (m_stats.fillMode == VoxelFillMode::Solid) ? UINT64_MAX
                                                                                : cells.CountNonZeroMismatches(m_grid);

        m_cells = std::move(cells);
        m_cellStats = stats;
        m_cellsGeneration = m_generation;
        return true;
    }
}
