#include "Cases/StreamCase.h"
#include "Cases/PerfMeshSource.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Renderer/Voxel/ChunkSignedDistance.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"
#include "Renderer/Voxel/VoxelRegion.h"

#include <algorithm>
#include <cfloat>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        // Ticks before the streamer gives up on loading every wanted chunk.
        constexpr uint32_t kMaxTicks = 4096;
        // Wall-time budget of one streaming run, under the 5 s debug-command timeout.
        constexpr double kStreamBudgetMs = 4000.0;

        double Kib(uint64_t bytes)
        {
            return static_cast<double>(bytes) / 1024.0;
        }

        Render::Voxel::VoxelizerMeshInput SoupInput(const Render::Voxel::WorldTriangleSoup& soup)
        {
            Render::Voxel::VoxelizerMeshInput mesh;
            mesh.positions = soup.positions.data();
            mesh.vertexCount = static_cast<uint32_t>(soup.positions.size() / 3);
            mesh.positionStrideInFloats = 3;
            mesh.indices = soup.indices.data();
            mesh.indexCount = static_cast<uint32_t>(soup.indices.size());
            return mesh;
        }
    }

    StreamCase::StreamCase(std::string name, std::string assetPath, uint32_t divisions, uint32_t chunkVoxels,
                           uint32_t radius, uint32_t sdfApron)
        : m_name(std::move(name))
        , m_assetPath(std::move(assetPath))
        , m_divisions((std::max)(divisions, 8u))
    {
        m_settings.chunkVoxels = chunkVoxels;
        m_settings.loadRadius = radius;
        m_settings.maxJobsInFlight = 4;
        m_settings.sdfApronVoxels = sdfApron;
        m_description = m_assetPath + ": full grid vs chunk streaming, " + std::to_string(m_divisions)
            + " voxels on the longest side, C=" + std::to_string(chunkVoxels) + " radius " + std::to_string(radius)
            + " sdf " + std::to_string(sdfApron) + " (cpuMs = voxelize[+EDT] vs stream until loaded)";
    }

    bool StreamCase::Prepare(std::string& error)
    {
        m_extras.clear();
        m_streamer.reset();
        m_streamComplete = false;
        m_fullMatches = true;

        PerfMeshSource source;
        if (!LoadWeldedMesh(m_assetPath, source, error)) {
            return false;
        }
        if (source.vertices.empty() || source.indices.empty()) {
            error = "empty mesh";
            return false;
        }

        auto soup = std::make_shared<Render::Voxel::WorldTriangleSoup>();
        soup->positions.reserve(source.vertices.size() * 3);
        for (uint32_t a = 0; a < 3; ++a) {
            soup->boundsMin[a] = FLT_MAX;
            soup->boundsMax[a] = -FLT_MAX;
        }
        for (const Render::Vertex& vertex : source.vertices) {
            for (uint32_t a = 0; a < 3; ++a) {
                soup->boundsMin[a] = (std::min)(soup->boundsMin[a], vertex.position[a]);
                soup->boundsMax[a] = (std::max)(soup->boundsMax[a], vertex.position[a]);
                soup->positions.push_back(vertex.position[a]);
            }
        }
        soup->indices = source.indices;
        soup->hasBounds = true;
        soup->drawItemsUsed = 1;
        const float longest = (std::max)({ soup->boundsMax[0] - soup->boundsMin[0],
            soup->boundsMax[1] - soup->boundsMin[1], soup->boundsMax[2] - soup->boundsMin[2] });
        if (!(longest > 0.0f)) {
            error = "degenerate bounds";
            return false;
        }
        m_settings.voxelSize = longest / static_cast<float>(m_divisions);
        for (uint32_t a = 0; a < 3; ++a) {
            m_camera[a] = 0.5f * (soup->boundsMin[a] + soup->boundsMax[a]);
        }
        m_soup = soup;

        // The anchor grid is the streamer's; one throwaway start reads it.
        Render::Voxel::VoxelChunkStreamer probe;
        if (!probe.StartFromSoup(m_soup, m_settings, error)) {
            return false;
        }
        m_desc = probe.AnchorDesc();
        const Render::Voxel::VoxelChunkStreamerStats& probeStats = probe.Stats();
        const double chunkCount = static_cast<double>(probeStats.chunkCountX) * probeStats.chunkCountY * probeStats.chunkCountZ;
        probe.Stop();

        m_fullGrid.Reset(m_desc.dimX, m_desc.dimY, m_desc.dimZ);
        Render::Voxel::VoxelizeSurface(m_desc, SoupInput(*m_soup), m_fullGrid);
        if (m_settings.sdfApronVoxels > 0) {
            Render::Voxel::BuildSignedDistanceField(m_fullGrid, m_fullSdf);
        }

        m_extras.push_back({ "voxelSize", m_settings.voxelSize });
        m_extras.push_back({ "dimX", static_cast<double>(m_desc.dimX) });
        m_extras.push_back({ "dimY", static_cast<double>(m_desc.dimY) });
        m_extras.push_back({ "dimZ", static_cast<double>(m_desc.dimZ) });
        m_extras.push_back({ "chunks", chunkCount });
        m_extras.push_back({ "triangles", static_cast<double>(m_soup->indices.size() / 3) });
        return true;
    }

    PerfMetrics StreamCase::RunVariant(PerfVariant variant)
    {
        return variant == PerfVariant::Baseline ? RunFull() : RunStream();
    }

    PerfMetrics StreamCase::RunFull()
    {
        PerfMetrics metrics;
        Render::Voxel::OccupancyBitGrid grid;
        Render::Voxel::SignedDistanceGrid sdf;
        const auto start = Foundation::ScopedPerfTimer::Now();
        grid.Reset(m_desc.dimX, m_desc.dimY, m_desc.dimZ);
        Render::Voxel::VoxelizeSurface(m_desc, SoupInput(*m_soup), grid);
        const double voxelizeMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        if (m_settings.sdfApronVoxels > 0) {
            Render::Voxel::BuildSignedDistanceField(grid, sdf);
        }
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        metrics.cpuBytes = grid.SizeInBytes() + (m_settings.sdfApronVoxels > 0 ? sdf.SizeInBytes() : 0);
        if (grid.Words() != m_fullGrid.Words()) {
            m_fullMatches = false;
        }
        metrics.extras = m_extras;
        metrics.extras.push_back({ "voxelizeMs", voxelizeMs });
        metrics.extras.push_back({ "edtMs", metrics.cpuMs - voxelizeMs });
        metrics.extras.push_back({ "fullKB", Kib(metrics.cpuBytes) });
        return metrics;
    }

    PerfMetrics StreamCase::RunStream()
    {
        PerfMetrics metrics;
        m_streamer.reset();
        auto streamer = std::make_unique<Render::Voxel::VoxelChunkStreamer>();
        std::string error;
        const auto start = Foundation::ScopedPerfTimer::Now();
        const bool started = streamer->StartFromSoup(m_soup, m_settings, error);
        const double startMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        uint32_t ticks = 0;
        m_streamComplete = false;
        if (started) {
            for (; ticks < kMaxTicks; ++ticks) {
                streamer->Tick(m_camera);
                streamer->WaitIdle();
                const Render::Voxel::VoxelChunkStreamerStats& stats = streamer->Stats();
                if (stats.loading == 0 && stats.loaded == stats.wanted) {
                    m_streamComplete = true;
                    break;
                }
                if (Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now()) > kStreamBudgetMs) {
                    break;
                }
            }
        }
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        const Render::Voxel::VoxelChunkStreamerStats& stats = streamer->Stats();
        metrics.cpuBytes = stats.residentBytes;
        metrics.extras = m_extras;
        metrics.extras.push_back({ "startMs", startMs });
        metrics.extras.push_back({ "ticks", static_cast<double>(ticks) });
        metrics.extras.push_back({ "loaded", static_cast<double>(stats.loaded) });
        metrics.extras.push_back({ "wanted", static_cast<double>(stats.wanted) });
        metrics.extras.push_back({ "loads", static_cast<double>(stats.loadsCompleted) });
        metrics.extras.push_back({ "lastJobMs", stats.lastJobMs });
        metrics.extras.push_back({ "maxJobMs", stats.maxJobMs });
        metrics.extras.push_back({ "sumGatherMs", stats.sumGatherMs });
        metrics.extras.push_back({ "sumVoxelizeMs", stats.sumVoxelizeMs });
        metrics.extras.push_back({ "sumSdfMs", stats.sumSdfMs });
        metrics.extras.push_back({ "sumJobMs", stats.sumJobMs });
        metrics.extras.push_back({ "jobTriangles", static_cast<double>(stats.sumJobTriangles) });
        metrics.extras.push_back({ "regionVoxels", static_cast<double>(stats.sumRegionVoxels) });
        // Wall time spread over the jobs that ran; empty chunks load without a job.
        metrics.extras.push_back({ "meanWallPerLoadMs",
            stats.loadsCompleted > 0 ? (metrics.cpuMs - startMs) / static_cast<double>(stats.loadsCompleted) : 0.0 });
        metrics.extras.push_back({ "residentKB", Kib(stats.residentBytes) });
        metrics.extras.push_back({ "residentSdfKB", Kib(stats.residentSdfBytes) });
        metrics.extras.push_back({ "complete", m_streamComplete ? 1.0 : 0.0 });
        m_streamer = std::move(streamer);
        return metrics;
    }

    bool StreamCase::Check(std::string& detail)
    {
        if (!m_fullMatches) {
            detail = "full voxelization is not deterministic";
            return false;
        }
        if (!m_streamer || !m_streamComplete) {
            detail = "streamer did not load every wanted chunk";
            return false;
        }
        const bool withSdf = m_settings.sdfApronVoxels > 0;
        const float limit = withSdf ? Render::Voxel::ChunkSdfLimit(m_settings.sdfApronVoxels) : 0.0f;
        uint64_t chunks = 0;
        uint64_t mismatch = 0;
        uint64_t sdfMismatch = 0;
        uint64_t badShape = 0;
        m_streamer->ForEachChunk([&](const Render::Voxel::VoxelChunkStreamer::ChunkView& view) {
            ++chunks;
            const Render::Voxel::VoxelRegion region = m_streamer->ChunkRegion(view.coord[0], view.coord[1], view.coord[2]);
            if (!view.grid || view.grid->DimX() != region.size[0] || view.grid->DimY() != region.size[1]
                || view.grid->DimZ() != region.size[2]) {
                ++badShape;
                return;
            }
            const bool sdfOk = !withSdf || (view.sdf && view.sdf->DimX() == region.size[0]
                && view.sdf->DimY() == region.size[1] && view.sdf->DimZ() == region.size[2]);
            if (!sdfOk) {
                ++badShape;
                return;
            }
            for (uint32_t z = 0; z < region.size[2]; ++z) {
                for (uint32_t y = 0; y < region.size[1]; ++y) {
                    for (uint32_t x = 0; x < region.size[0]; ++x) {
                        const uint32_t gx = region.min[0] + x;
                        const uint32_t gy = region.min[1] + y;
                        const uint32_t gz = region.min[2] + z;
                        if (view.grid->Test(x, y, z) != m_fullGrid.Test(gx, gy, gz)) {
                            ++mismatch;
                        }
                        if (withSdf && view.sdf->Get(x, y, z) != std::clamp(m_fullSdf.Get(gx, gy, gz), -limit, limit)) {
                            ++sdfMismatch;
                        }
                    }
                }
            }
        });
        detail = "chunks " + std::to_string(chunks) + " mismatch " + std::to_string(mismatch)
            + (withSdf ? " sdfMismatch " + std::to_string(sdfMismatch) : std::string())
            + (badShape > 0 ? " badShape " + std::to_string(badShape) : std::string());
        return chunks > 0 && mismatch == 0 && sdfMismatch == 0 && badShape == 0;
    }

    void StreamCase::Release()
    {
        m_streamer.reset();
        m_soup.reset();
        m_fullGrid.Reset(0, 0, 0);
        m_fullSdf.Reset(0, 0, 0);
        m_extras.clear();
    }
}
