#pragma once

// Content-addressed on-disk cache of built ClusterDags, so the expensive cluster/LOD
// build (ClusterDagBuilder) runs once per distinct mesh + build-settings combination.

#include "Renderer/VirtualGeometry/ClusterDag.h"

#include <cstdint>
#include <filesystem>

namespace SasamiEngine::Render
{
    struct Mesh;

    // <directory>/<key as 16 hex>.sdag
    class ClusterDagCache
    {
    public:
        explicit ClusterDagCache(std::filesystem::path directory);

        // FNV-1a 64 over: the raw Vertex bytes, the raw index bytes, the settings fields, and
        // ClusterDagSerializer::kFormatVersion. Identical input => identical key on any run.
        static uint64_t ComputeKey(const Mesh& mesh, const ClusterDagBuildSettings& settings);

        bool TryLoad(uint64_t key, ClusterDag& outDag) const;   // false on miss or corrupt file
        bool Store(uint64_t key, const ClusterDag& dag) const;  // creates directory; temp+rename

        const std::filesystem::path& GetDirectory() const { return m_directory; }

    private:
        std::filesystem::path PathFor(uint64_t key) const;

        std::filesystem::path m_directory;
    };
}

namespace SasamiEngine { using namespace Render; }
