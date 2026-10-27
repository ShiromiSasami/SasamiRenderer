#pragma once

#include "PerfCase.h"

#include <cstdint>
#include <string>

namespace SasamiEngine::Sample::PerfLab
{
    // Static model load with and without StaticMeshCache. Baseline parses and converts the source
    // file (LoadStaticModelUncached: tinygltf / ufbx / OBJ, triangulation, dedup); Optimized reads
    // the .smesh cache (StaticMeshCache::Load). The vertex-cache reorder and triangle stats that
    // LoadStaticModel adds after both are left out, as they cost the same either way. Prepare writes
    // the cache if it is missing or stale (one parse). Runs on the main thread, because ufbx crashed
    // on worker threads; a Bistro run blocks the frame for its whole duration.
    // cpuMs = loadSec * 1000, cpuBytes = vertex + index bytes produced, diskBytes = the source file
    // (plus .bin buffers next to a .gltf; textures are not read by the loader) or the cache file.
    // The OS file cache is warm after the first run, so warm runs measure CPU work, not the disk.
    // Check: both variants produce the same meshes (a digest of vertices, indices, texture paths
    // and transforms).
    class MeshLoadCase : public IPerfCase
    {
    public:
        MeshLoadCase(std::string name, std::string assetPath);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }

        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        std::string m_name;
        std::string m_assetPath;
        std::string m_fullPath;
        std::string m_description;
        uint32_t m_format = 0;          // App::StaticModelFormat
        uint64_t m_sourceBytes = 0;
        uint64_t m_cacheBytes = 0;
        uint64_t m_digest[2] = {};
        uint64_t m_meshCount[2] = {};
        uint32_t m_failed[2] = {};
        uint32_t m_runs[2] = {};
    };
}
