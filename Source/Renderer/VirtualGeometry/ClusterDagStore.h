#pragma once

#include "Renderer/VirtualGeometry/ClusterDag.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    struct Mesh;

    struct ClusterDagStoreStats
    {
        uint32_t meshCount = 0;     // meshes submitted to Build
        uint32_t builtCount = 0;    // built by ClusterDagBuilder this call
        uint32_t cacheHitCount = 0; // loaded from ClusterDagCache
        uint32_t emptyCount = 0;    // meshes that yielded an empty DAG (no triangles etc.)
        uint32_t skippedCount = 0;  // meshes left empty on purpose (Build's skipMeshes)
        double   buildSeconds = 0.0;
    };

    // Owns one ClusterDag per submitted mesh (same indexing as SceneSubmitter::GetMeshes()).
    class ClusterDagStore
    {
    public:
        // skipMeshes (same indexing as meshes, nonzero = skip) leaves those DAGs empty, which keeps the
        // meshes out of every cluster range; SceneSubmitter passes its dynamic-vertex flags.
        void Build(const std::vector<Mesh>& meshes, const ClusterDagBuildSettings& settings,
                   const std::filesystem::path& cacheDirectory,
                   const std::vector<uint8_t>* skipMeshes = nullptr);
        void Release();

        bool IsBuilt() const;
        size_t GetMeshCount() const;
        const ClusterDag* GetDag(size_t meshIndex) const; // nullptr when out of range or empty
        const ClusterDagBuildSettings& GetSettings() const;
        const ClusterDagStoreStats& GetStats() const;

        std::string FormatReport() const;

    private:
        std::vector<std::unique_ptr<ClusterDag>> m_dags;
        ClusterDagBuildSettings                  m_settings{};
        ClusterDagStoreStats                     m_stats{};
        bool                                      m_built = false;
    };
}

namespace SasamiEngine { using namespace Render; }
