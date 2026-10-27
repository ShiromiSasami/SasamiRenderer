#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterDagStore.h"

#include "Renderer/VirtualGeometry/ClusterDagBuilder.h"
#include "Renderer/VirtualGeometry/ClusterDagCache.h"
#include "Renderer/Structures/Mesh.h"

#include "Foundation/Jobs/ParallelFor.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace SasamiEngine::Render
{
    namespace
    {
        std::string FormatFixed3(double value)
        {
            char buffer[64] = {};
            std::snprintf(buffer, sizeof(buffer), "%.3f", value);
            return buffer;
        }
    }

    void ClusterDagStore::Build(const std::vector<Mesh>& meshes, const ClusterDagBuildSettings& settings,
                                const std::filesystem::path& cacheDirectory,
                                const std::vector<uint8_t>* skipMeshes)
    {
        Release();

        ScopedPerfTimer timer("ClusterDagStore::Build");
        const auto startTime = std::chrono::steady_clock::now();

        m_settings = settings;
        const uint32_t meshCount = static_cast<uint32_t>(meshes.size());
        m_dags.resize(meshCount);
        for (auto& dag : m_dags) {
            dag = std::make_unique<ClusterDag>();
        }

        ClusterDagCache cache(cacheDirectory);
        // An unset directory means "no cache": build every DAG and do not try to store, instead
        // of failing (and logging) once per mesh from inside the worker threads.
        const bool useCache = !cacheDirectory.empty();
        if (!useCache) {
            DebugLog("ClusterDagStore::Build: cache directory not set, building without cache.\n");
        }
        std::atomic<uint32_t> builtCount{ 0 };
        std::atomic<uint32_t> cacheHitCount{ 0 };
        std::atomic<uint32_t> emptyCount{ 0 };
        std::atomic<uint32_t> skippedCount{ 0 };

        ParallelFor(meshCount, [&](uint32_t i) {
            ClusterDag& dag = *m_dags[i];
            if (skipMeshes && i < skipMeshes->size() && (*skipMeshes)[i] != 0) {
                ++skippedCount;
                return;
            }
            const uint64_t key = ClusterDagCache::ComputeKey(meshes[i], settings);
            if (useCache && cache.TryLoad(key, dag)) {
                ++cacheHitCount;
            } else {
                dag = ClusterDagBuilder::Build(meshes[i], settings);
                if (!dag.IsEmpty()) {
                    if (useCache) {
                        cache.Store(key, dag);
                    }
                }
                ++builtCount;
            }
            if (dag.IsEmpty()) {
                ++emptyCount;
            }
        });

        const auto endTime = std::chrono::steady_clock::now();

        m_stats.meshCount     = meshCount;
        m_stats.builtCount    = builtCount.load();
        m_stats.cacheHitCount = cacheHitCount.load();
        m_stats.emptyCount    = emptyCount.load();
        m_stats.skippedCount  = skippedCount.load();
        m_stats.buildSeconds  = std::chrono::duration<double>(endTime - startTime).count();
        m_built = true;
    }

    void ClusterDagStore::Release()
    {
        m_dags.clear();
        m_settings = ClusterDagBuildSettings{};
        m_stats    = ClusterDagStoreStats{};
        m_built    = false;
    }

    bool ClusterDagStore::IsBuilt() const
    {
        return m_built;
    }

    size_t ClusterDagStore::GetMeshCount() const
    {
        return m_dags.size();
    }

    const ClusterDag* ClusterDagStore::GetDag(size_t meshIndex) const
    {
        if (meshIndex >= m_dags.size() || !m_dags[meshIndex] || m_dags[meshIndex]->IsEmpty()) {
            return nullptr;
        }
        return m_dags[meshIndex].get();
    }

    const ClusterDagBuildSettings& ClusterDagStore::GetSettings() const
    {
        return m_settings;
    }

    const ClusterDagStoreStats& ClusterDagStore::GetStats() const
    {
        return m_stats;
    }

    std::string ClusterDagStore::FormatReport() const
    {
        if (!m_built) {
            return "ERR cluster DAGs not built";
        }

        uint32_t maxLevelCount    = 0;
        uint32_t terminalGroups   = 0;
        uint64_t sourceTris       = 0;
        size_t   largestMeshIndex = 0;
        uint32_t largestSourceTris = 0;
        uint32_t largestLevels    = 0;

        for (size_t i = 0; i < m_dags.size(); ++i) {
            const ClusterDag* dag = m_dags[i].get();
            if (!dag) {
                continue;
            }
            maxLevelCount = std::max(maxLevelCount, dag->levelCount);
            terminalGroups += dag->terminalGroupCount;
            sourceTris += dag->sourceTriangleCount;
            if (dag->sourceTriangleCount > largestSourceTris) {
                largestSourceTris = dag->sourceTriangleCount;
                largestMeshIndex  = i;
                largestLevels     = dag->levelCount;
            }
        }

        std::vector<uint64_t> levelClusters(maxLevelCount, 0);
        std::vector<uint64_t> levelTriangles(maxLevelCount, 0);
        for (const auto& dagPtr : m_dags) {
            const ClusterDag* dag = dagPtr.get();
            if (!dag) {
                continue;
            }
            for (const ClusterDagCluster& cluster : dag->clusters) {
                const uint32_t level = dag->groups[cluster.group].depth;
                if (level >= maxLevelCount) {
                    continue;
                }
                ++levelClusters[level];
                levelTriangles[level] += cluster.indexCount / 3u;
            }
        }
        const uint64_t lod0Clusters = maxLevelCount > 0 ? levelClusters[0] : 0;

        std::string out = "OK meshes=" + std::to_string(m_stats.meshCount)
            + " built=" + std::to_string(m_stats.builtCount)
            + " cacheHits=" + std::to_string(m_stats.cacheHitCount)
            + " empty=" + std::to_string(m_stats.emptyCount)
            + " seconds=" + FormatFixed3(m_stats.buildSeconds)
            + " sourceTris=" + std::to_string(sourceTris)
            + " lod0Clusters=" + std::to_string(lod0Clusters)
            + " levels=" + std::to_string(maxLevelCount)
            + " terminalGroups=" + std::to_string(terminalGroups);

        for (uint32_t level = 0; level < maxLevelCount; ++level) {
            out += " L" + std::to_string(level) + "=" + std::to_string(levelClusters[level])
                 + "/" + std::to_string(levelTriangles[level]);
        }

        out += " largest=" + std::to_string(largestMeshIndex) + ":" + std::to_string(largestSourceTris)
             + ":" + std::to_string(largestLevels);

        return out;
    }
}
