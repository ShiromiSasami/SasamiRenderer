#include "Cases/MeshLoadCase.h"

#include "ApplicationResourcePaths.h"
#include "Loader/ModelLoader.h"
#include "Loader/StaticMeshCache.h"
#include "Foundation/Tools/ScopedPerfTimer.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        constexpr float kUniformScale = 1.0f;

        bool FormatFromPath(const std::string& path, App::StaticModelFormat& format)
        {
            std::string ext = std::filesystem::path(path).extension().string();
            for (char& c : ext) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (ext == ".obj") {
                format = App::StaticModelFormat::Obj;
            } else if (ext == ".gltf" || ext == ".glb") {
                format = App::StaticModelFormat::Gltf;
            } else if (ext == ".fbx") {
                format = App::StaticModelFormat::Fbx;
            } else {
                return false;
            }
            return true;
        }

        uint64_t FileBytes(const std::filesystem::path& path)
        {
            std::error_code ec;
            const uintmax_t size = std::filesystem::file_size(path, ec);
            return ec ? 0 : static_cast<uint64_t>(size);
        }

        // The source file plus, for .gltf, the .bin buffers in its directory.
        uint64_t SourceBytes(const std::filesystem::path& path)
        {
            uint64_t total = FileBytes(path);
            if (path.extension() == ".gltf") {
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), ec)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                        total += FileBytes(entry.path());
                    }
                }
            }
            return total;
        }

        // FNV-1a 64.
        void Mix(uint64_t& hash, const void* data, size_t size)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
        }

        uint64_t Digest(const std::vector<App::LoadedStaticMesh>& meshes)
        {
            uint64_t hash = 14695981039346656037ull;
            for (const App::LoadedStaticMesh& m : meshes) {
                Mix(hash, m.mesh.vertices.data(), m.mesh.vertices.size() * sizeof(Render::Vertex));
                Mix(hash, m.mesh.indices.data(), m.mesh.indices.size() * sizeof(uint32_t));
                Mix(hash, m.localTransform, sizeof(m.localTransform));
                for (const std::string* path : { &m.texturePath, &m.occlusionTexturePath, &m.normalTexturePath,
                         &m.metallicRoughnessTexturePath, &m.emissiveTexturePath }) {
                    Mix(hash, path->data(), path->size());
                    Mix(hash, "\0", 1);
                }
            }
            return hash;
        }

        uint64_t GeometryBytes(const std::vector<App::LoadedStaticMesh>& meshes)
        {
            uint64_t total = 0;
            for (const App::LoadedStaticMesh& m : meshes) {
                total += m.mesh.vertices.size() * sizeof(Render::Vertex) + m.mesh.indices.size() * sizeof(uint32_t);
            }
            return total;
        }
    }

    MeshLoadCase::MeshLoadCase(std::string name, std::string assetPath)
        : m_name(std::move(name))
        , m_assetPath(std::move(assetPath))
    {
        m_description = "parse " + m_assetPath + " vs read its StaticMeshCache";
    }

    bool MeshLoadCase::Prepare(std::string& error)
    {
        App::StaticModelFormat format;
        if (!FormatFromPath(m_assetPath, format)) {
            error = "unsupported model format: " + m_assetPath;
            return false;
        }
        m_format = static_cast<uint32_t>(format);
        m_fullPath = App::ApplicationResourcePaths::ResolveAssetPathString(m_assetPath);
        m_sourceBytes = SourceBytes(m_fullPath);
        if (m_sourceBytes == 0) {
            error = "missing " + m_fullPath;
            return false;
        }

        const std::filesystem::path cachePath = App::StaticMeshCache::ResolveCachePath(m_fullPath, kUniformScale);
        if (!App::StaticMeshCache::IsUpToDate(cachePath, m_fullPath, kUniformScale)) {
            std::vector<App::LoadedStaticMesh> meshes;
            if (!App::LoadStaticModelUncached(m_fullPath, format, kUniformScale, meshes) || meshes.empty() ||
                !App::StaticMeshCache::Save(cachePath, m_fullPath, kUniformScale, meshes)) {
                error = "could not build the cache for " + m_fullPath;
                return false;
            }
        }
        m_cacheBytes = FileBytes(cachePath);
        for (uint32_t v = 0; v < 2; ++v) {
            m_digest[v] = 0;
            m_meshCount[v] = 0;
            m_failed[v] = 0;
            m_runs[v] = 0;
        }
        return true;
    }

    PerfMetrics MeshLoadCase::RunVariant(PerfVariant variant)
    {
        const uint32_t v = static_cast<uint32_t>(variant);
        const auto format = static_cast<App::StaticModelFormat>(m_format);
        std::vector<App::LoadedStaticMesh> meshes;

        const auto start = Foundation::ScopedPerfTimer::Now();
        bool ok = false;
        if (variant == PerfVariant::Baseline) {
            ok = App::LoadStaticModelUncached(m_fullPath, format, kUniformScale, meshes);
        } else {
            const std::filesystem::path cachePath = App::StaticMeshCache::ResolveCachePath(m_fullPath, kUniformScale);
            ok = App::StaticMeshCache::Load(cachePath, meshes);
        }
        const double ms = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());

        ++m_runs[v];
        if (!ok || meshes.empty()) {
            ++m_failed[v];
        } else {
            m_digest[v] = Digest(meshes);
            m_meshCount[v] = meshes.size();
        }

        PerfMetrics metrics;
        metrics.cpuMs = ms;
        metrics.loadSec = ms / 1000.0;
        metrics.cpuBytes = GeometryBytes(meshes);
        metrics.diskBytes = (variant == PerfVariant::Baseline) ? m_sourceBytes : m_cacheBytes;
        metrics.extras.push_back({ "meshes", static_cast<double>(meshes.size()) });
        return metrics;
    }

    bool MeshLoadCase::Check(std::string& detail)
    {
        char text[256];
        const bool same = m_failed[0] == 0 && m_failed[1] == 0 && m_runs[0] > 0 && m_runs[1] > 0 &&
            m_digest[0] == m_digest[1] && m_meshCount[0] == m_meshCount[1];
        std::snprintf(text, sizeof(text), "meshes=%llu/%llu failed=%u/%u digest %s",
            static_cast<unsigned long long>(m_meshCount[0]), static_cast<unsigned long long>(m_meshCount[1]),
            m_failed[0], m_failed[1], m_digest[0] == m_digest[1] ? "equal" : "DIFFERS");
        detail = text;
        return same;
    }

    void MeshLoadCase::Release()
    {
    }
}
