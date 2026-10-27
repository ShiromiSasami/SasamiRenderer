#include "Cases/BrickGridCase.h"

#include "Cases/PerfMeshSource.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Renderer/Voxel/CpuSolidVoxelizer.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"

#include <algorithm>
#include <cfloat>
#include <intrin.h>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        constexpr uint32_t kQueryCount = 4u << 20;
        constexpr uint32_t kNearRadius = 2;

        // PCG32 (O'Neill 2014), so the query list is the same on every run.
        struct Pcg32 {
            uint64_t state = 0x853C49E6748FEA9Bull;
            uint32_t Next()
            {
                const uint64_t old = state;
                state = old * 6364136223846793005ull + 1442695040888963407ull;
                const uint32_t xorShifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
                const uint32_t rot = static_cast<uint32_t>(old >> 59u);
                return (xorShifted >> rot) | (xorShifted << ((32u - rot) & 31u));
            }
            uint32_t Below(uint32_t bound) { return static_cast<uint32_t>((static_cast<uint64_t>(Next()) * bound) >> 32); }
        };

        uint32_t PackQuery(uint32_t x, uint32_t y, uint32_t z)
        {
            return x | (y << 10) | (z << 20);
        }

        template <class Grid>
        __declspec(noinline) uint64_t CountHits(const Grid& grid, const std::vector<uint32_t>& queries)
        {
            uint64_t hits = 0;
            for (uint32_t q : queries) {
                hits += grid.Test(q & 1023u, (q >> 10) & 1023u, q >> 20) ? 1u : 0u;
            }
            return hits;
        }

        double Kib(uint64_t bytes)
        {
            return static_cast<double>(bytes) / 1024.0;
        }
    }

    BrickGridCase::BrickGridCase(std::string name, std::string assetPath, uint32_t maxDim, bool solid)
        : m_name(std::move(name))
        , m_assetPath(std::move(assetPath))
        , m_maxDim((std::min)(maxDim, 1024u))
        , m_solid(solid)
    {
        m_description = m_assetPath + ": dense bit grid vs 8^3 bricks, " + std::to_string(m_maxDim)
            + (m_solid ? " solid" : " surface") + " (cpuMs = " + std::to_string(kQueryCount >> 20)
            + "M Test queries; size/build in extras)";
    }

    bool BrickGridCase::Prepare(std::string& error)
    {
        m_extras.clear();
        m_queries.clear();
        for (uint32_t v = 0; v < 2; ++v) {
            m_hits[v] = 0;
            m_ran[v] = false;
        }
        m_decodeOk = false;

        PerfMeshSource source;
        if (!LoadWeldedMesh(m_assetPath, source, error)) {
            return false;
        }
        if (source.vertices.empty() || source.indices.empty()) {
            error = "empty mesh";
            return false;
        }

        float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        std::vector<float> positions;
        positions.reserve(source.vertices.size() * 3);
        for (const Render::Vertex& vertex : source.vertices) {
            for (uint32_t a = 0; a < 3; ++a) {
                lo[a] = (std::min)(lo[a], vertex.position[a]);
                hi[a] = (std::max)(hi[a], vertex.position[a]);
                positions.push_back(vertex.position[a]);
            }
        }
        const float longest = (std::max)({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });
        if (!(longest > 0.0f)) {
            error = "degenerate bounds";
            return false;
        }

        // One voxel of padding on each side, so the surface never touches the grid edge.
        Render::Voxel::VoxelGridDesc desc;
        desc.voxelSize = longest / static_cast<float>(m_maxDim - 2);
        uint32_t* dims[3] = { &desc.dimX, &desc.dimY, &desc.dimZ };
        for (uint32_t a = 0; a < 3; ++a) {
            desc.origin[a] = lo[a] - desc.voxelSize;
            const uint32_t n = static_cast<uint32_t>((hi[a] - lo[a]) / desc.voxelSize) + 3u;
            *dims[a] = (std::min)(n, m_maxDim);
        }

        Render::Voxel::VoxelizerMeshInput mesh;
        mesh.positions = positions.data();
        mesh.vertexCount = static_cast<uint32_t>(source.vertices.size());
        mesh.positionStrideInFloats = 3;
        mesh.indices = source.indices.data();
        mesh.indexCount = static_cast<uint32_t>(source.indices.size());

        const auto voxelStart = Foundation::ScopedPerfTimer::Now();
        m_dense.Reset(desc.dimX, desc.dimY, desc.dimZ);
        Render::Voxel::VoxelizeSurface(desc, mesh, m_dense);
        if (m_solid) {
            Render::Voxel::OccupancyBitGrid interior(desc.dimX, desc.dimY, desc.dimZ);
            Render::Voxel::VoxelizeSolidParity(desc, mesh, interior);
            m_dense.OrWith(interior);
        }
        const double voxelizeMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(voxelStart, Foundation::ScopedPerfTimer::Now());

        const auto buildStart = Foundation::ScopedPerfTimer::Now();
        m_bricks.Build(m_dense);
        const double buildMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(buildStart, Foundation::ScopedPerfTimer::Now());

        Render::Voxel::OccupancyBitGrid decoded;
        const auto decodeStart = Foundation::ScopedPerfTimer::Now();
        m_bricks.Decode(decoded);
        const double decodeMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(decodeStart, Foundation::ScopedPerfTimer::Now());
        m_decodeOk = decoded.DimX() == m_dense.DimX() && decoded.DimY() == m_dense.DimY()
            && decoded.DimZ() == m_dense.DimZ() && decoded.Words() == m_dense.Words();

        // Occupied voxels, for the near-surface half of the queries.
        std::vector<uint64_t> occupied;
        const std::vector<uint32_t>& words = m_dense.Words();
        for (size_t w = 0; w < words.size(); ++w) {
            for (uint32_t bits = words[w]; bits != 0; bits &= bits - 1) {
                unsigned long bit = 0;
                _BitScanForward(&bit, bits);
                occupied.push_back(static_cast<uint64_t>(w) * 32 + bit);
            }
        }

        const uint32_t dimX = desc.dimX;
        const uint32_t dimY = desc.dimY;
        const uint32_t dimZ = desc.dimZ;
        const uint64_t slice = static_cast<uint64_t>(dimX) * dimY;
        Pcg32 rng;
        m_queries.reserve(kQueryCount);
        for (uint32_t i = 0; i < kQueryCount; ++i) {
            if ((i & 1u) == 0 || occupied.empty()) {
                m_queries.push_back(PackQuery(rng.Below(dimX), rng.Below(dimY), rng.Below(dimZ)));
                continue;
            }
            const uint64_t linear = occupied[static_cast<size_t>(rng.Below(static_cast<uint32_t>((std::min)(occupied.size(), size_t(0xFFFFFFFFu)))))];
            const int64_t base[3] = {
                static_cast<int64_t>(linear % dimX),
                static_cast<int64_t>((linear / dimX) % dimY),
                static_cast<int64_t>(linear / slice) };
            const uint32_t dimsArr[3] = { dimX, dimY, dimZ };
            uint32_t c[3];
            for (uint32_t a = 0; a < 3; ++a) {
                const int64_t offset = static_cast<int64_t>(rng.Below(2 * kNearRadius + 1)) - kNearRadius;
                c[a] = static_cast<uint32_t>(std::clamp<int64_t>(base[a] + offset, 0, dimsArr[a] - 1));
            }
            m_queries.push_back(PackQuery(c[0], c[1], c[2]));
        }

        const uint64_t denseBytes = m_dense.SizeInBytes();
        const uint64_t brickBytes = m_bricks.SizeInBytes();
        m_extras = {
            { "dimX", static_cast<double>(dimX) },
            { "dimY", static_cast<double>(dimY) },
            { "dimZ", static_cast<double>(dimZ) },
            { "occupied", static_cast<double>(occupied.size()) },
            { "denseKB", Kib(denseBytes) },
            { "brickKB", Kib(brickBytes) },
            { "ratio", brickBytes > 0 ? static_cast<double>(denseBytes) / static_cast<double>(brickBytes) : 0.0 },
            { "fullBricks", static_cast<double>(m_bricks.FullBrickCount()) },
            { "mixedBricks", static_cast<double>(m_bricks.MixedBrickCount()) },
            { "bricks", static_cast<double>(m_bricks.BrickCount()) },
            { "voxelizeMs", voxelizeMs },
            { "buildMs", buildMs },
            { "decodeMs", decodeMs },
        };
        return true;
    }

    PerfMetrics BrickGridCase::RunVariant(PerfVariant variant)
    {
        const uint32_t v = static_cast<uint32_t>(variant);
        PerfMetrics metrics;
        const auto start = Foundation::ScopedPerfTimer::Now();
        const uint64_t hits = variant == PerfVariant::Baseline ? CountHits(m_dense, m_queries) : CountHits(m_bricks, m_queries);
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        metrics.cpuBytes = variant == PerfVariant::Baseline ? m_dense.SizeInBytes() : m_bricks.SizeInBytes();
        metrics.extras = m_extras;
        metrics.extras.push_back({ "hits", static_cast<double>(hits) });
        if (m_ran[v] && m_hits[v] != hits) {
            m_hits[v] = ~0ull;   // not deterministic: Check reports it
        } else {
            m_hits[v] = hits;
        }
        m_ran[v] = true;
        return metrics;
    }

    bool BrickGridCase::Check(std::string& detail)
    {
        if (!m_decodeOk) {
            detail = "Decode differs from the dense grid";
            return false;
        }
        if (!m_ran[0] || !m_ran[1] || m_hits[0] != m_hits[1] || m_hits[0] == ~0ull) {
            detail = "hit counts differ: baseline " + std::to_string(m_hits[0]) + " optimized " + std::to_string(m_hits[1]);
            return false;
        }
        detail = "decode bit-identical, hits " + std::to_string(m_hits[0]) + "/" + std::to_string(m_queries.size());
        return true;
    }

    void BrickGridCase::Release()
    {
        m_dense.Reset(0, 0, 0);
        m_bricks = Render::Voxel::BrickOccupancyGrid{};
        m_queries.clear();
        m_queries.shrink_to_fit();
    }
}
