#include "Renderer/VirtualGeometry/ClusterDagCache.h"

#include "Renderer/VirtualGeometry/ClusterDagSerializer.h"
#include "Renderer/Structures/Mesh.h"
#include "Foundation/Tools/DebugOutput.h"

#include <cstdio>
#include <fstream>
#include <system_error>

namespace SasamiEngine::Render
{
    namespace
    {
        constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
        constexpr uint64_t kFnvPrime = 1099511628211ull;

        void FnvHashBytes(uint64_t& hash, const void* data, size_t byteCount)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < byteCount; ++i) {
                hash ^= static_cast<uint64_t>(bytes[i]);
                hash *= kFnvPrime;
            }
        }

        template <typename T>
        void FnvHashValue(uint64_t& hash, const T& value)
        {
            FnvHashBytes(hash, &value, sizeof(T));
        }
    }

    ClusterDagCache::ClusterDagCache(std::filesystem::path directory)
        : m_directory(std::move(directory))
    {
    }

    uint64_t ClusterDagCache::ComputeKey(const Mesh& mesh, const ClusterDagBuildSettings& settings)
    {
        uint64_t hash = kFnvOffsetBasis;

        if (!mesh.vertices.empty()) {
            FnvHashBytes(hash, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
        }
        if (!mesh.indices.empty()) {
            FnvHashBytes(hash, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
        }

        FnvHashValue(hash, settings.maxTriangles);
        FnvHashValue(hash, settings.maxVertices);
        FnvHashValue(hash, settings.partitionSize);
        FnvHashValue(hash, settings.simplifyRatio);
        FnvHashValue(hash, settings.hierarchyNodeWidth);

        FnvHashValue(hash, ClusterDagSerializer::kFormatVersion);

        return hash;
    }

    std::filesystem::path ClusterDagCache::PathFor(uint64_t key) const
    {
        char name[32]{};
        snprintf(name, sizeof(name), "%016llx.sdag", static_cast<unsigned long long>(key));
        return m_directory / name;
    }

    bool ClusterDagCache::TryLoad(uint64_t key, ClusterDag& outDag) const
    {
        const std::filesystem::path path = PathFor(key);

        std::error_code ec;
        const auto fileSize = std::filesystem::file_size(path, ec);
        if (ec) {
            return false; // Plain miss: not worth logging.
        }

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }

        std::vector<std::byte> bytes(static_cast<size_t>(fileSize));
        if (fileSize > 0) {
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fileSize));
        }
        if (!file) {
            std::error_code removeEc;
            std::filesystem::remove(path, removeEc);
            DebugLog(("ClusterDagCache: failed to read \"" + path.string() + "\"\n").c_str());
            return false;
        }
        file.close();

        if (!ClusterDagSerializer::Deserialize(bytes, outDag)) {
            std::error_code removeEc;
            std::filesystem::remove(path, removeEc);
            DebugLog(("ClusterDagCache: corrupt cache entry removed \"" + path.string() + "\"\n").c_str());
            return false;
        }

        return true;
    }

    bool ClusterDagCache::Store(uint64_t key, const ClusterDag& dag) const
    {
        std::error_code ec;
        std::filesystem::create_directories(m_directory, ec);
        if (ec) {
            DebugLog(("ClusterDagCache: cannot create cache directory \"" + m_directory.string() + "\"\n").c_str());
            return false;
        }

        const std::filesystem::path path = PathFor(key);
        std::filesystem::path tempPath = path;
        tempPath += ".tmp";

        const std::vector<std::byte> bytes = ClusterDagSerializer::Serialize(dag);

        {
            std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
            if (!file) {
                DebugLog(("ClusterDagCache: cannot open \"" + tempPath.string() + "\" for writing\n").c_str());
                return false;
            }

            if (!bytes.empty()) {
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }

            if (!file) {
                file.close();
                std::filesystem::remove(tempPath, ec);
                DebugLog(("ClusterDagCache: write failed for \"" + tempPath.string() + "\"\n").c_str());
                return false;
            }
        }

        std::filesystem::remove(path, ec);
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            std::error_code cleanupEc;
            std::filesystem::remove(tempPath, cleanupEc);
            DebugLog(("ClusterDagCache: cannot publish \"" + path.string() + "\"\n").c_str());
            return false;
        }

        return true;
    }
}
