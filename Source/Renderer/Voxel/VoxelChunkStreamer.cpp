#include "Renderer/Voxel/VoxelChunkStreamer.h"

#include "Renderer/Voxel/ChunkSignedDistance.h"
#include "Renderer/Voxel/StreamedVoxelWindow.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"

#include "Foundation/Jobs/JobCounter.h"
#include "Foundation/Jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace SasamiEngine::Render::Voxel
{
    struct VoxelChunkStreamer::Chunk
    {
        uint32_t coord[3] = { 0, 0, 0 };
        VoxelChunkState state = VoxelChunkState::Loading;
        OccupancyBitGrid grid;
        SignedDistanceGrid sdf;   // empty unless sdfApronVoxels > 0
    };

    struct VoxelChunkStreamer::Job
    {
        uint32_t key = 0;
        uint32_t coord[3] = { 0, 0, 0 };
        VoxelGridDesc desc{};
        VoxelRegion region{};              // voxelized region: the chunk plus its apron
        uint32_t coreOffset[3] = { 0, 0, 0 };   // the chunk inside region
        uint32_t coreSize[3] = { 0, 0, 0 };
        uint32_t apronVoxels = 0;
        std::shared_ptr<const WorldTriangleSoup> soup;
        const VoxelChunkStreamer* owner = nullptr;
        std::vector<uint32_t> triangles;   // triangles of every chunk overlapping region
        OccupancyBitGrid result;           // the chunk only
        SignedDistanceGrid sdf;
        double ms = 0.0;
        double gatherMs = 0.0;
        double voxelizeMs = 0.0;
        double sdfMs = 0.0;
        Foundation::JobCounter counter;
    };

    namespace
    {
        bool SettingsValid(const VoxelChunkStreamerSettings& settings, std::string& outError)
        {
            if (!(settings.voxelSize > 0.0f))
            {
                outError = "voxelSize must be > 0";
                return false;
            }
            if (settings.chunkVoxels < 4)
            {
                outError = "chunkVoxels must be >= 4";
                return false;
            }
            if (settings.maxJobsInFlight < 1)
            {
                outError = "maxJobsInFlight must be >= 1";
                return false;
            }
            return true;
        }

        // Anchor grid: scene bounds floored to voxelSize and padded; dims stay tight (edge chunks are clamped).
        bool BuildAnchor(const WorldTriangleSoup& soup, const VoxelChunkStreamerSettings& settings,
                         VoxelGridDesc& outDesc, uint32_t outChunkCount[3], std::string& outError)
        {
            const double vs = static_cast<double>(settings.voxelSize);
            const double pad = static_cast<double>(settings.padVoxels);
            uint32_t n[3] = { 0, 0, 0 };
            outDesc = VoxelGridDesc{};
            outDesc.voxelSize = settings.voxelSize;
            for (int i = 0; i < 3; ++i)
            {
                const double origin = std::floor(static_cast<double>(soup.boundsMin[i]) / vs) * vs - pad * vs;
                const double count = std::ceil((static_cast<double>(soup.boundsMax[i]) + pad * vs - origin) / vs);
                if (!(count < 1.0e7))
                {
                    outError = "grid too large, raise voxel=";
                    return false;
                }
                outDesc.origin[i] = static_cast<float>(origin);
                n[i] = static_cast<uint32_t>((std::max)(count, 1.0));
                outChunkCount[i] = (n[i] + settings.chunkVoxels - 1) / settings.chunkVoxels;
            }
            outDesc.dimX = n[0];
            outDesc.dimY = n[1];
            outDesc.dimZ = n[2];
            const uint64_t total = static_cast<uint64_t>(outChunkCount[0]) * outChunkCount[1] * outChunkCount[2];
            if (total >= (1ull << 24))
            {
                outError = "too many chunks, raise voxel=";
                return false;
            }
            return true;
        }

        // Voxels triangle t can write (its AABB padded by one voxel); false for a bad index or a miss.
        // Bucketing and the per-job filter both use it, so they agree on which triangles touch a region.
        bool TriangleVoxelRegion(const WorldTriangleSoup& soup, const VoxelGridDesc& desc, size_t t, VoxelRegion& outRegion)
        {
            const uint32_t vertexCount = static_cast<uint32_t>(soup.positions.size() / 3);
            float bmin[3];
            float bmax[3];
            for (int v = 0; v < 3; ++v)
            {
                const uint32_t index = soup.indices[t * 3 + v];
                if (index >= vertexCount)
                {
                    return false;
                }
                const float* p = &soup.positions[static_cast<size_t>(index) * 3];
                for (int a = 0; a < 3; ++a)
                {
                    bmin[a] = v == 0 ? p[a] : (std::min)(bmin[a], p[a]);
                    bmax[a] = v == 0 ? p[a] : (std::max)(bmax[a], p[a]);
                }
            }
            return VoxelRegionFromWorldBounds(desc, bmin, bmax, 1, outRegion) && !outRegion.IsEmpty();
        }

        // outBoxes gets every bucketed triangle's TriangleVoxelRegion when all grid dims fit in 16 bits
        // (cleared otherwise; the gather then recomputes them).
        void BucketTriangles(const WorldTriangleSoup& soup, const VoxelGridDesc& desc, uint32_t chunkVoxels,
                             const uint32_t chunkCount[3], std::vector<std::vector<uint32_t>>& outBuckets,
                             std::vector<VoxelTriangleBox>& outBoxes)
        {
            outBuckets.assign(static_cast<size_t>(chunkCount[0]) * chunkCount[1] * chunkCount[2], {});
            const size_t triangleCount = soup.indices.size() / 3;
            const bool keepBoxes = desc.dimX <= 0xFFFFu && desc.dimY <= 0xFFFFu && desc.dimZ <= 0xFFFFu;
            outBoxes.clear();
            if (keepBoxes)
            {
                outBoxes.resize(triangleCount);
            }
            for (size_t t = 0; t < triangleCount; ++t)
            {
                VoxelRegion region;
                if (!TriangleVoxelRegion(soup, desc, t, region))
                {
                    continue;
                }
                if (keepBoxes)
                {
                    for (int a = 0; a < 3; ++a)
                    {
                        outBoxes[t].min[a] = static_cast<uint16_t>(region.min[a]);
                        outBoxes[t].max[a] = static_cast<uint16_t>(region.min[a] + region.size[a] - 1);
                    }
                }
                uint32_t lo[3];
                uint32_t hi[3];
                for (int a = 0; a < 3; ++a)
                {
                    lo[a] = region.min[a] / chunkVoxels;
                    hi[a] = (std::min)((region.min[a] + region.size[a] - 1) / chunkVoxels, chunkCount[a] - 1);
                }
                for (uint32_t cz = lo[2]; cz <= hi[2]; ++cz)
                {
                    for (uint32_t cy = lo[1]; cy <= hi[1]; ++cy)
                    {
                        for (uint32_t cx = lo[0]; cx <= hi[0]; ++cx)
                        {
                            const size_t key = static_cast<size_t>(cx) + static_cast<size_t>(cy) * chunkCount[0]
                                + static_cast<size_t>(cz) * chunkCount[0] * chunkCount[1];
                            outBuckets[key].push_back(static_cast<uint32_t>(t));
                        }
                    }
                }
            }
        }

        // Shared by the worker jobs and the selftest.
        void VoxelizeChunk(const VoxelGridDesc& desc, const VoxelRegion& region, const WorldTriangleSoup& soup,
                           const std::vector<uint32_t>& triangles, OccupancyBitGrid& outGrid)
        {
            outGrid.Reset(region.size[0], region.size[1], region.size[2]);
            if (triangles.empty())
            {
                return;
            }
            std::vector<uint32_t> local;
            local.reserve(triangles.size() * 3);
            for (const uint32_t t : triangles)
            {
                local.push_back(soup.indices[static_cast<size_t>(t) * 3 + 0]);
                local.push_back(soup.indices[static_cast<size_t>(t) * 3 + 1]);
                local.push_back(soup.indices[static_cast<size_t>(t) * 3 + 2]);
            }
            VoxelizerMeshInput input;
            input.positions = soup.positions.data();
            input.vertexCount = static_cast<uint32_t>(soup.positions.size() / 3);
            input.positionStrideInFloats = 3;
            input.indices = local.data();
            input.indexCount = static_cast<uint32_t>(local.size());
            VoxelizeSurfaceRegion(desc, region, input, outGrid);
        }

        void AddTriangle(WorldTriangleSoup& soup, const float a[3], const float b[3], const float c[3])
        {
            const uint32_t base = static_cast<uint32_t>(soup.positions.size() / 3);
            for (const float* p : { a, b, c })
            {
                for (int i = 0; i < 3; ++i)
                {
                    soup.positions.push_back(p[i]);
                    if (!soup.hasBounds)
                    {
                        soup.boundsMin[i] = p[i];
                        soup.boundsMax[i] = p[i];
                    }
                    soup.boundsMin[i] = (std::min)(soup.boundsMin[i], p[i]);
                    soup.boundsMax[i] = (std::max)(soup.boundsMax[i], p[i]);
                }
                soup.hasBounds = true;
            }
            soup.indices.push_back(base);
            soup.indices.push_back(base + 1);
            soup.indices.push_back(base + 2);
        }
    }

    VoxelChunkStreamer::VoxelChunkStreamer() = default;

    VoxelChunkStreamer::~VoxelChunkStreamer()
    {
        Stop();
    }

    uint32_t VoxelChunkStreamer::ChunkKey(uint32_t cx, uint32_t cy, uint32_t cz) const
    {
        return cx + cy * m_chunkCount[0] + cz * m_chunkCount[0] * m_chunkCount[1];
    }

    bool VoxelChunkStreamer::Start(const std::vector<SceneSubmitter::DrawItem>& drawItems, const std::vector<Mesh>& meshes,
                                   const VoxelChunkStreamerSettings& settings, std::string& outError)
    {
        Stop();
        if (!SettingsValid(settings, outError))
        {
            return false;
        }
        auto soup = std::make_shared<WorldTriangleSoup>(GatherOpaqueWorldTriangles(drawItems, meshes));
        return StartFromSoup(std::move(soup), settings, outError);
    }

    bool VoxelChunkStreamer::StartFromSoup(std::shared_ptr<const WorldTriangleSoup> soup,
                                           const VoxelChunkStreamerSettings& settings, std::string& outError)
    {
        Stop();
        if (!SettingsValid(settings, outError))
        {
            return false;
        }
        if (!soup || !soup->hasBounds)
        {
            outError = "no opaque triangles";
            return false;
        }
        VoxelGridDesc desc;
        uint32_t chunkCount[3] = { 0, 0, 0 };
        if (!BuildAnchor(*soup, settings, desc, chunkCount, outError))
        {
            return false;
        }

        m_settings = settings;
        m_desc = desc;
        std::memcpy(m_chunkCount, chunkCount, sizeof(m_chunkCount));
        m_soup = std::move(soup);
        BucketTriangles(*m_soup, m_desc, m_settings.chunkVoxels, m_chunkCount, m_chunkTriangles, m_triangleBoxes);
        m_stats = VoxelChunkStreamerStats{};
        m_stats.chunkCountX = m_chunkCount[0];
        m_stats.chunkCountY = m_chunkCount[1];
        m_stats.chunkCountZ = m_chunkCount[2];
        m_stats.triangles = m_soup->indices.size() / 3;
        m_running = true;
        return true;
    }

    void VoxelChunkStreamer::Stop()
    {
        WaitIdle();
        m_chunks.clear();
        m_jobs.clear();
        m_chunkTriangles.clear();
        m_triangleBoxes.clear();
        m_soup.reset();
        m_stats = VoxelChunkStreamerStats{};
        m_running = false;
    }

    VoxelRegion VoxelChunkStreamer::ChunkRegion(uint32_t cx, uint32_t cy, uint32_t cz) const
    {
        VoxelRegion region;
        const uint32_t c[3] = { cx, cy, cz };
        const uint32_t dims[3] = { m_desc.dimX, m_desc.dimY, m_desc.dimZ };
        const uint32_t chunkVoxels = m_settings.chunkVoxels;
        for (int a = 0; a < 3; ++a)
        {
            if (c[a] >= m_chunkCount[a])
            {
                return VoxelRegion{};
            }
            region.min[a] = c[a] * chunkVoxels;
            region.size[a] = (std::min)(chunkVoxels, dims[a] - region.min[a]);
        }
        return region;
    }

    VoxelRegion VoxelChunkStreamer::ExpandedChunkRegion(uint32_t cx, uint32_t cy, uint32_t cz) const
    {
        VoxelRegion region = ChunkRegion(cx, cy, cz);
        if (region.IsEmpty())
        {
            return region;
        }
        const uint32_t dims[3] = { m_desc.dimX, m_desc.dimY, m_desc.dimZ };
        const uint32_t apron = m_settings.sdfApronVoxels;
        for (int a = 0; a < 3; ++a)
        {
            const uint32_t lo = region.min[a] > apron ? region.min[a] - apron : 0;
            const uint64_t hiWide = static_cast<uint64_t>(region.min[a]) + region.size[a] + apron;
            const uint32_t hi = static_cast<uint32_t>((std::min)(hiWide, static_cast<uint64_t>(dims[a])));
            region.min[a] = lo;
            region.size[a] = hi - lo;
        }
        return region;
    }

    void VoxelChunkStreamer::ChunkRange(const VoxelRegion& region, uint32_t outLo[3], uint32_t outHi[3]) const
    {
        const uint32_t chunkVoxels = m_settings.chunkVoxels;
        for (int a = 0; a < 3; ++a)
        {
            outLo[a] = region.min[a] / chunkVoxels;
            outHi[a] = (std::min)((region.min[a] + region.size[a] - 1) / chunkVoxels, m_chunkCount[a] - 1);
        }
    }

    bool VoxelChunkStreamer::RegionHasTriangles(const VoxelRegion& region) const
    {
        if (region.IsEmpty())
        {
            return false;
        }
        uint32_t lo[3];
        uint32_t hi[3];
        ChunkRange(region, lo, hi);
        for (uint32_t cz = lo[2]; cz <= hi[2]; ++cz)
        {
            for (uint32_t cy = lo[1]; cy <= hi[1]; ++cy)
            {
                for (uint32_t cx = lo[0]; cx <= hi[0]; ++cx)
                {
                    if (!m_chunkTriangles[ChunkKey(cx, cy, cz)].empty())
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void VoxelChunkStreamer::GatherRegionTriangles(const VoxelRegion& region, std::vector<uint32_t>& outTriangles) const
    {
        outTriangles.clear();
        if (region.IsEmpty())
        {
            return;
        }
        uint32_t lo[3];
        uint32_t hi[3];
        ChunkRange(region, lo, hi);
        for (uint32_t cz = lo[2]; cz <= hi[2]; ++cz)
        {
            for (uint32_t cy = lo[1]; cy <= hi[1]; ++cy)
            {
                for (uint32_t cx = lo[0]; cx <= hi[0]; ++cx)
                {
                    // A bucket holds every triangle touching its chunk; one touching only the part of the
                    // chunk outside region writes nothing inside it, so it is dropped here. A triangle that
                    // does touch region is taken only from the first chunk of its range inside [lo, hi],
                    // so each one is pushed once.
                    for (uint32_t t : m_chunkTriangles[ChunkKey(cx, cy, cz)])
                    {
                        uint32_t boxMin[3];
                        uint32_t boxMax[3];
                        if (!m_triangleBoxes.empty())
                        {
                            for (int a = 0; a < 3; ++a)
                            {
                                boxMin[a] = m_triangleBoxes[t].min[a];
                                boxMax[a] = m_triangleBoxes[t].max[a];
                            }
                        }
                        else
                        {
                            VoxelRegion triangleRegion;
                            TriangleVoxelRegion(*m_soup, m_desc, t, triangleRegion);
                            for (int a = 0; a < 3; ++a)
                            {
                                boxMin[a] = triangleRegion.min[a];
                                boxMax[a] = triangleRegion.min[a] + triangleRegion.size[a] - 1;
                            }
                        }
                        bool keep = true;
                        const uint32_t chunk[3] = { cx, cy, cz };
                        for (int a = 0; a < 3 && keep; ++a)
                        {
                            const uint32_t regionMax = region.min[a] + region.size[a] - 1;
                            keep = boxMin[a] <= regionMax && boxMax[a] >= region.min[a]
                                && chunk[a] == (std::max)(lo[a], boxMin[a] / m_settings.chunkVoxels);
                        }
                        if (keep)
                        {
                            outTriangles.push_back(t);
                        }
                    }
                }
            }
        }
        // Same order as before the per-chunk dedupe (ascending index).
        std::sort(outTriangles.begin(), outTriangles.end());
    }

    void VoxelChunkStreamer::WorldToChunk(const float position[3], int32_t outChunk[3]) const
    {
        const double vs = static_cast<double>(m_desc.voxelSize);
        for (int a = 0; a < 3; ++a)
        {
            if (m_chunkCount[a] == 0 || !(vs > 0.0))
            {
                outChunk[a] = 0;
                continue;
            }
            const double voxel = std::floor((static_cast<double>(position[a]) - static_cast<double>(m_desc.origin[a])) / vs);
            const double chunk = std::floor(voxel / static_cast<double>(m_settings.chunkVoxels));
            outChunk[a] = static_cast<int32_t>(std::clamp(chunk, 0.0, static_cast<double>(m_chunkCount[a] - 1)));
        }
    }

    void VoxelChunkStreamer::Tick(const float cameraPosition[3])
    {
        if (!m_running)
        {
            return;
        }
        CollectFinished();

        int32_t cam[3];
        WorldToChunk(cameraPosition, cam);
        for (int a = 0; a < 3; ++a)
        {
            m_stats.cameraChunk[a] = cam[a];
        }

        const int32_t keepDistance = static_cast<int32_t>(m_settings.loadRadius + m_settings.unloadSlack);
        for (auto it = m_chunks.begin(); it != m_chunks.end();)
        {
            const Chunk& chunk = *it->second;
            int32_t distance = 0;
            for (int a = 0; a < 3; ++a)
            {
                distance = (std::max)(distance, std::abs(static_cast<int32_t>(chunk.coord[a]) - cam[a]));
            }
            if (distance > keepDistance)
            {
                if (chunk.state == VoxelChunkState::Loaded)
                {
                    ++m_stats.unloadsCompleted;
                }
                it = m_chunks.erase(it);
            }
            else
            {
                ++it;
            }
        }

        struct Wanted
        {
            int32_t distance;
            int64_t distanceSq;
            uint32_t coord[3];
        };
        std::vector<Wanted> wanted;
        const int32_t radius = static_cast<int32_t>(m_settings.loadRadius);
        int32_t lo[3];
        int32_t hi[3];
        for (int a = 0; a < 3; ++a)
        {
            lo[a] = (std::max)(cam[a] - radius, 0);
            hi[a] = (std::min)(cam[a] + radius, static_cast<int32_t>(m_chunkCount[a]) - 1);
        }
        for (int32_t z = lo[2]; z <= hi[2]; ++z)
        {
            for (int32_t y = lo[1]; y <= hi[1]; ++y)
            {
                for (int32_t x = lo[0]; x <= hi[0]; ++x)
                {
                    const int32_t dx = x - cam[0];
                    const int32_t dy = y - cam[1];
                    const int32_t dz = z - cam[2];
                    Wanted w;
                    w.distance = (std::max)({ std::abs(dx), std::abs(dy), std::abs(dz) });
                    w.distanceSq = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy + static_cast<int64_t>(dz) * dz;
                    w.coord[0] = static_cast<uint32_t>(x);
                    w.coord[1] = static_cast<uint32_t>(y);
                    w.coord[2] = static_cast<uint32_t>(z);
                    wanted.push_back(w);
                }
            }
        }
        std::sort(wanted.begin(), wanted.end(), [](const Wanted& a, const Wanted& b)
        {
            if (a.distance != b.distance)
            {
                return a.distance < b.distance;
            }
            return a.distanceSq < b.distanceSq;
        });
        m_stats.wanted = static_cast<uint32_t>(wanted.size());

        for (const Wanted& w : wanted)
        {
            const uint32_t key = ChunkKey(w.coord[0], w.coord[1], w.coord[2]);
            if (m_chunks.find(key) != m_chunks.end())
            {
                continue;
            }
            if (!RegionHasTriangles(ExpandedChunkRegion(w.coord[0], w.coord[1], w.coord[2])))
            {
                auto chunk = std::make_unique<Chunk>();
                std::memcpy(chunk->coord, w.coord, sizeof(chunk->coord));
                chunk->state = VoxelChunkState::Loaded;
                const VoxelRegion region = ChunkRegion(w.coord[0], w.coord[1], w.coord[2]);
                chunk->grid.Reset(region.size[0], region.size[1], region.size[2]);
                if (m_settings.sdfApronVoxels > 0)
                {
                    FillEmptyChunkSignedDistance(region.size, m_settings.sdfApronVoxels, chunk->sdf);
                }
                m_chunks.emplace(key, std::move(chunk));
                continue;
            }
            if (m_jobs.size() < m_settings.maxJobsInFlight)
            {
                KickLoad(w.coord[0], w.coord[1], w.coord[2]);
            }
        }

        RefreshResidentStats();
    }

    void VoxelChunkStreamer::KickLoad(uint32_t cx, uint32_t cy, uint32_t cz)
    {
        const uint32_t key = ChunkKey(cx, cy, cz);

        auto chunk = std::make_unique<Chunk>();
        chunk->coord[0] = cx;
        chunk->coord[1] = cy;
        chunk->coord[2] = cz;
        chunk->state = VoxelChunkState::Loading;
        m_chunks[key] = std::move(chunk);

        auto job = std::make_unique<Job>();
        job->key = key;
        job->coord[0] = cx;
        job->coord[1] = cy;
        job->coord[2] = cz;
        job->desc = m_desc;
        const VoxelRegion core = ChunkRegion(cx, cy, cz);
        job->region = ExpandedChunkRegion(cx, cy, cz);
        for (int a = 0; a < 3; ++a)
        {
            job->coreOffset[a] = core.min[a] - job->region.min[a];
            job->coreSize[a] = core.size[a];
        }
        job->apronVoxels = m_settings.sdfApronVoxels;
        job->soup = m_soup;
        // The job gathers its own triangles: the buckets, desc and settings only change in Start/Stop,
        // and Stop waits for every job first.
        job->owner = this;
        job->counter.Reset(1);
        Foundation::JobSystem::Kick(&VoxelChunkStreamer::RunJob, job.get(), &job->counter);
        m_jobs.push_back(std::move(job));
    }

    void VoxelChunkStreamer::RunJob(void* userdata)
    {
        Job* job = static_cast<Job*>(userdata);
        const auto gatherStart = std::chrono::steady_clock::now();
        job->owner->GatherRegionTriangles(job->region, job->triangles);
        const auto start = std::chrono::steady_clock::now();
        job->gatherMs = std::chrono::duration<double, std::milli>(start - gatherStart).count();
        if (job->apronVoxels == 0)
        {
            VoxelizeChunk(job->desc, job->region, *job->soup, job->triangles, job->result);
        }
        else
        {
            OccupancyBitGrid expanded;
            VoxelizeChunk(job->desc, job->region, *job->soup, job->triangles, expanded);
            const auto sdfStart = std::chrono::steady_clock::now();
            job->voxelizeMs = std::chrono::duration<double, std::milli>(sdfStart - start).count();
            BuildChunkSignedDistance(expanded, job->coreOffset, job->coreSize, job->apronVoxels, job->result, job->sdf);
            job->sdfMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sdfStart).count();
        }
        const auto end = std::chrono::steady_clock::now();
        job->ms = std::chrono::duration<double, std::milli>(end - gatherStart).count();
        if (job->apronVoxels == 0)
        {
            job->voxelizeMs = std::chrono::duration<double, std::milli>(end - start).count();
        }
    }

    void VoxelChunkStreamer::CollectFinished()
    {
        for (size_t i = 0; i < m_jobs.size();)
        {
            Job& job = *m_jobs[i];
            if (job.counter.Get() != 0)
            {
                ++i;
                continue;
            }
            auto it = m_chunks.find(job.key);
            if (it != m_chunks.end() && it->second->state == VoxelChunkState::Loading)
            {
                it->second->grid = std::move(job.result);
                it->second->sdf = std::move(job.sdf);
                it->second->state = VoxelChunkState::Loaded;
                ++m_stats.loadsCompleted;
                m_stats.lastJobMs = job.ms;
                m_stats.maxJobMs = (std::max)(m_stats.maxJobMs, job.ms);
                m_stats.sumGatherMs += job.gatherMs;
                m_stats.sumVoxelizeMs += job.voxelizeMs;
                m_stats.sumSdfMs += job.sdfMs;
                m_stats.sumJobMs += job.ms;
                m_stats.sumJobTriangles += job.triangles.size();
                m_stats.sumRegionVoxels += static_cast<uint64_t>(job.region.size[0]) * job.region.size[1] * job.region.size[2];
            }
            else
            {
                ++m_stats.discardedResults;
            }
            m_jobs.erase(m_jobs.begin() + static_cast<std::ptrdiff_t>(i));
        }

        RefreshResidentStats();
    }

    void VoxelChunkStreamer::RefreshResidentStats()
    {
        m_stats.loaded = 0;
        m_stats.residentBytes = 0;
        m_stats.residentSdfBytes = 0;
        for (const auto& entry : m_chunks)
        {
            if (entry.second->state == VoxelChunkState::Loaded)
            {
                ++m_stats.loaded;
                m_stats.residentBytes += entry.second->grid.SizeInBytes() + entry.second->sdf.SizeInBytes();
                m_stats.residentSdfBytes += entry.second->sdf.SizeInBytes();
            }
        }
        m_stats.loading = static_cast<uint32_t>(m_jobs.size());
    }

    void VoxelChunkStreamer::WaitIdle()
    {
        for (const auto& job : m_jobs)
        {
            job->counter.Wait();
        }
        CollectFinished();
    }

    void VoxelChunkStreamer::ForEachChunk(const std::function<void(const ChunkView&)>& fn) const
    {
        for (const auto& entry : m_chunks)
        {
            const Chunk& chunk = *entry.second;
            ChunkView view;
            std::memcpy(view.coord, chunk.coord, sizeof(view.coord));
            view.state = chunk.state;
            view.grid = chunk.state == VoxelChunkState::Loaded ? &chunk.grid : nullptr;
            view.sdf = chunk.state == VoxelChunkState::Loaded && chunk.sdf.VoxelCount() != 0 ? &chunk.sdf : nullptr;
            fn(view);
        }
    }

    bool VoxelChunkStreamer::SelfTest(std::string& outReport)
    {
        auto soup = std::make_shared<WorldTriangleSoup>();
        {
            // Tilted quad spanning several chunks.
            const float q0[3] = { 1.0f, 0.5f, 1.0f };
            const float q1[3] = { 5.0f, 3.0f, 1.5f };
            const float q2[3] = { 5.5f, 3.5f, 5.5f };
            const float q3[3] = { 1.5f, 1.0f, 5.0f };
            AddTriangle(*soup, q0, q1, q2);
            AddTriangle(*soup, q0, q2, q3);
            // Small triangle lying exactly on the chunk boundary plane x = 2.0 (8 voxels * 0.25).
            const float b0[3] = { 2.0f, 1.0f, 3.0f };
            const float b1[3] = { 2.0f, 1.4f, 3.0f };
            const float b2[3] = { 2.0f, 1.0f, 3.4f };
            AddTriangle(*soup, b0, b1, b2);
            // Long thin triangle crossing many chunks.
            const float l0[3] = { 0.3f, 0.3f, 8.0f };
            const float l1[3] = { 9.5f, 5.5f, 8.3f };
            const float l2[3] = { 9.5f, 5.6f, 8.3f };
            AddTriangle(*soup, l0, l1, l2);
        }

        VoxelChunkStreamerSettings settings;
        settings.voxelSize = 0.25f;
        settings.chunkVoxels = 8;
        settings.loadRadius = 1;
        settings.unloadSlack = 0;
        settings.maxJobsInFlight = 4;
        settings.padVoxels = 1;

        VoxelChunkStreamer streamer;
        std::string error;
        if (!streamer.StartFromSoup(soup, settings, error))
        {
            outReport = "ERR voxel.stream selftest start failed: " + error;
            return false;
        }
        const VoxelGridDesc desc = streamer.AnchorDesc();

        OccupancyBitGrid fullGrid(desc.dimX, desc.dimY, desc.dimZ);
        VoxelizerMeshInput fullInput;
        fullInput.positions = soup->positions.data();
        fullInput.vertexCount = static_cast<uint32_t>(soup->positions.size() / 3);
        fullInput.positionStrideInFloats = 3;
        fullInput.indices = soup->indices.data();
        fullInput.indexCount = static_cast<uint32_t>(soup->indices.size());
        VoxelizeSurface(desc, fullInput, fullGrid);
        const uint64_t setCount = fullGrid.CountOccupied();

        // Pass 1: every chunk voxelized synchronously and assembled.
        OccupancyBitGrid assembled(desc.dimX, desc.dimY, desc.dimZ);
        uint32_t chunkTotal = 0;
        for (uint32_t cz = 0; cz < streamer.m_chunkCount[2]; ++cz)
        {
            for (uint32_t cy = 0; cy < streamer.m_chunkCount[1]; ++cy)
            {
                for (uint32_t cx = 0; cx < streamer.m_chunkCount[0]; ++cx)
                {
                    const VoxelRegion region = streamer.ChunkRegion(cx, cy, cz);
                    OccupancyBitGrid chunkGrid;
                    VoxelizeChunk(desc, region, *soup, streamer.m_chunkTriangles[streamer.ChunkKey(cx, cy, cz)], chunkGrid);
                    assembled.WriteRegion(chunkGrid, region.min);
                    ++chunkTotal;
                }
            }
        }
        uint64_t mismatch = 0;
        for (uint32_t z = 0; z < desc.dimZ; ++z)
        {
            for (uint32_t y = 0; y < desc.dimY; ++y)
            {
                for (uint32_t x = 0; x < desc.dimX; ++x)
                {
                    if (assembled.Test(x, y, z) != fullGrid.Test(x, y, z))
                    {
                        ++mismatch;
                    }
                }
            }
        }

        // Pass 2: the streaming path around a camera point, then a far point for unloads.
        uint64_t streamMismatch = 0;
        const float nearPoint[3] = { 5.0f, 3.0f, 5.0f };
        const float farPoint[3] = { soup->boundsMax[0] + 100.0f, soup->boundsMax[1] + 100.0f, soup->boundsMax[2] + 100.0f };
        for (int i = 0; i < 64; ++i)
        {
            streamer.Tick(nearPoint);
            streamer.WaitIdle();
            if (streamer.Stats().loading == 0 && streamer.Stats().loaded == streamer.Stats().wanted)
            {
                break;
            }
        }
        if (streamer.Stats().loaded != streamer.Stats().wanted)
        {
            outReport = "ERR voxel.stream selftest streaming loaded=" + std::to_string(streamer.Stats().loaded)
                + " wanted=" + std::to_string(streamer.Stats().wanted);
            return false;
        }
        streamer.ForEachChunk([&](const ChunkView& view)
        {
            if (view.grid == nullptr)
            {
                ++streamMismatch;
                return;
            }
            const VoxelRegion region = streamer.ChunkRegion(view.coord[0], view.coord[1], view.coord[2]);
            for (uint32_t z = 0; z < region.size[2]; ++z)
            {
                for (uint32_t y = 0; y < region.size[1]; ++y)
                {
                    for (uint32_t x = 0; x < region.size[0]; ++x)
                    {
                        if (view.grid->Test(x, y, z) != fullGrid.Test(region.min[0] + x, region.min[1] + y, region.min[2] + z))
                        {
                            ++streamMismatch;
                        }
                    }
                }
            }
        });
        streamer.Tick(farPoint);
        streamer.WaitIdle();
        const uint64_t unloads = streamer.Stats().unloadsCompleted;

        // Pass 3: per-chunk SDF (apron 3) against the clamped SDF of the full grid. Every chunk is
        // streamed in, so empty chunks (FillEmptyChunkSignedDistance) are covered too.
        VoxelChunkStreamerSettings sdfSettings = settings;
        sdfSettings.sdfApronVoxels = 3;
        sdfSettings.loadRadius = 64;
        VoxelChunkStreamer sdfStreamer;
        if (!sdfStreamer.StartFromSoup(soup, sdfSettings, error))
        {
            outReport = "ERR voxel.stream selftest sdf start failed: " + error;
            return false;
        }
        SignedDistanceGrid fullSdf;
        BuildSignedDistanceField(fullGrid, fullSdf);
        const float limit = ChunkSdfLimit(sdfSettings.sdfApronVoxels);
        for (int i = 0; i < 256; ++i)
        {
            sdfStreamer.Tick(nearPoint);
            sdfStreamer.WaitIdle();
            if (sdfStreamer.Stats().loading == 0 && sdfStreamer.Stats().loaded == sdfStreamer.Stats().wanted)
            {
                break;
            }
        }
        uint64_t sdfMismatch = 0;
        uint32_t sdfChunks = 0;
        sdfStreamer.ForEachChunk([&](const ChunkView& view)
        {
            const VoxelRegion region = sdfStreamer.ChunkRegion(view.coord[0], view.coord[1], view.coord[2]);
            if (view.sdf == nullptr || view.sdf->DimX() != region.size[0] || view.sdf->DimY() != region.size[1]
                || view.sdf->DimZ() != region.size[2])
            {
                ++sdfMismatch;
                return;
            }
            ++sdfChunks;
            for (uint32_t z = 0; z < region.size[2]; ++z)
            {
                for (uint32_t y = 0; y < region.size[1]; ++y)
                {
                    for (uint32_t x = 0; x < region.size[0]; ++x)
                    {
                        const float expected = std::clamp(
                            fullSdf.Get(region.min[0] + x, region.min[1] + y, region.min[2] + z), -limit, limit);
                        if (view.sdf->Get(x, y, z) != expected)
                        {
                            ++sdfMismatch;
                        }
                    }
                }
            }
        });
        if (sdfChunks != chunkTotal)
        {
            ++sdfMismatch;
        }

        // Pass 4: radius-1 windows assembled from the SDF streamer against the same box of the full
        // grid and of its clamped SDF: one around nearPoint, one clamped at the grid corner.
        uint64_t windowMismatch = 0;
        int32_t nearChunk[3] = { 0, 0, 0 };
        sdfStreamer.WorldToChunk(nearPoint, nearChunk);
        const int32_t cornerChunk[3] = { 0, 0, 0 };
        for (const int32_t* centre : { static_cast<const int32_t*>(nearChunk), cornerChunk })
        {
            StreamedVoxelWindow window;
            uint32_t missing = 0;
            if (!AssembleStreamedVoxelWindow(sdfStreamer, centre, 1, window, missing, error))
            {
                ++windowMismatch;
                continue;
            }
            for (uint32_t z = 0; z < window.region.size[2]; ++z)
            {
                for (uint32_t y = 0; y < window.region.size[1]; ++y)
                {
                    for (uint32_t x = 0; x < window.region.size[0]; ++x)
                    {
                        const uint32_t gx = window.region.min[0] + x;
                        const uint32_t gy = window.region.min[1] + y;
                        const uint32_t gz = window.region.min[2] + z;
                        const float expected = std::clamp(fullSdf.Get(gx, gy, gz), -limit, limit);
                        if (window.occupancy.Test(x, y, z) != fullGrid.Test(gx, gy, gz)
                            || window.sdf.Get(x, y, z) != expected)
                        {
                            ++windowMismatch;
                        }
                    }
                }
            }
        }

        const std::string tail = " chunks=" + std::to_string(chunkTotal) + " mismatch=" + std::to_string(mismatch)
            + " streamMismatch=" + std::to_string(streamMismatch) + " unloads=" + std::to_string(unloads)
            + " set=" + std::to_string(setCount) + " sdfChunks=" + std::to_string(sdfChunks)
            + " sdfMismatch=" + std::to_string(sdfMismatch) + " windowMismatch=" + std::to_string(windowMismatch);
        if (mismatch != 0 || streamMismatch != 0 || setCount == 0 || unloads == 0 || sdfMismatch != 0
            || windowMismatch != 0)
        {
            outReport = "ERR voxel.stream selftest" + tail;
            return false;
        }
        outReport = "OK voxel.stream selftest" + tail;
        return true;
    }
}
