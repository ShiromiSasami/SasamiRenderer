#include "Loader/StaticMeshCache.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <type_traits>

#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    namespace StaticMeshCache
    {
        namespace
        {
            // Bulk reads/writes below reinterpret these structs directly, so a layout
            // change must invalidate every existing cache rather than silently corrupt
            // one. sizeof() is folded into the version for exactly that reason.
            static_assert(std::is_trivially_copyable_v<Vertex>, "Vertex must stay POD for bulk cache IO");
            static_assert(std::is_trivially_copyable_v<SurfaceMaterial>,
                          "SurfaceMaterial must stay POD for bulk cache IO");

            constexpr char kMagic[4] = { 'S', 'M', 'S', 'H' };
            // Bump the leading constant whenever the serialized field list changes:
            // v2 appends LoadedStaticMesh::normalTexturePath after the other texture paths.
            constexpr uint32_t kFormatVersion =
                2u ^ (static_cast<uint32_t>(sizeof(Vertex)) << 8) ^
                (static_cast<uint32_t>(sizeof(SurfaceMaterial)) << 16) ^
                (static_cast<uint32_t>(sizeof(uint32_t)) << 24);

            struct Header
            {
                char magic[4]{};
                uint32_t formatVersion = 0;
                uint64_t sourceFileSize = 0;
                int64_t sourceWriteTime = 0;
                float uniformScale = 0.0f;
                uint32_t submeshCount = 0;
            };

            std::filesystem::path GetExeDirectory()
            {
                wchar_t buffer[MAX_PATH]{};
                const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
                if (length == 0) {
                    return std::filesystem::path();
                }
                return std::filesystem::path(buffer).parent_path();
            }

            // Source paths become one flat filename: keeping separators would let an asset
            // path climb out of the cache directory.
            std::string SanitizeForFilename(const std::string& text)
            {
                std::string out;
                out.reserve(text.size());
                for (const char c : text) {
                    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                    out.push_back(safe ? c : '_');
                }
                return out;
            }

            bool ReadSourceStamp(const std::string& sourcePath, uint64_t& outSize, int64_t& outWriteTime)
            {
                std::error_code ec;
                const std::filesystem::path source(sourcePath);
                const auto size = std::filesystem::file_size(source, ec);
                if (ec) {
                    return false;
                }
                const auto writeTime = std::filesystem::last_write_time(source, ec);
                if (ec) {
                    return false;
                }
                outSize = static_cast<uint64_t>(size);
                outWriteTime = static_cast<int64_t>(writeTime.time_since_epoch().count());
                return true;
            }

            bool ReadHeader(std::ifstream& file, Header& outHeader)
            {
                file.read(reinterpret_cast<char*>(&outHeader), sizeof(Header));
                if (!file) {
                    return false;
                }
                return std::memcmp(outHeader.magic, kMagic, sizeof(kMagic)) == 0 &&
                       outHeader.formatVersion == kFormatVersion;
            }

            bool ReadString(std::ifstream& file, uint64_t remainingBytes, std::string& out)
            {
                uint32_t length = 0;
                file.read(reinterpret_cast<char*>(&length), sizeof(length));
                if (!file || static_cast<uint64_t>(length) > remainingBytes) {
                    return false;
                }
                out.resize(length);
                if (length > 0) {
                    file.read(out.data(), length);
                }
                return static_cast<bool>(file);
            }

            void WriteString(std::ofstream& file, const std::string& text)
            {
                const uint32_t length = static_cast<uint32_t>(text.size());
                file.write(reinterpret_cast<const char*>(&length), sizeof(length));
                if (length > 0) {
                    file.write(text.data(), length);
                }
            }
        }

        std::filesystem::path ResolveCachePath(const std::string& sourcePath, float uniformScale)
        {
            char scaleText[32]{};
            snprintf(scaleText, sizeof(scaleText), "%.6g", static_cast<double>(uniformScale));

            const std::string name = SanitizeForFilename(sourcePath) + "." +
                                     SanitizeForFilename(scaleText) + ".smesh";
            return GetExeDirectory() / L"AssetCache" / std::filesystem::path(name);
        }

        bool IsUpToDate(const std::filesystem::path& cachePath,
                        const std::string& sourcePath,
                        float uniformScale)
        {
            std::error_code ec;
            if (!std::filesystem::exists(cachePath, ec) || ec) {
                return false; // Plain miss: the normal first-run path, not worth logging.
            }

            uint64_t sourceSize = 0;
            int64_t sourceWriteTime = 0;
            if (!ReadSourceStamp(sourcePath, sourceSize, sourceWriteTime)) {
                return false;
            }

            std::ifstream file(cachePath, std::ios::binary);
            Header header;
            if (!file || !ReadHeader(file, header)) {
                return false;
            }

            // Bit-exact scale comparison so -0.0f / NaN can never alias another cache.
            return header.sourceFileSize == sourceSize &&
                   header.sourceWriteTime == sourceWriteTime &&
                   std::memcmp(&header.uniformScale, &uniformScale, sizeof(float)) == 0;
        }

        bool Load(const std::filesystem::path& cachePath, std::vector<LoadedStaticMesh>& outMeshes)
        {
            outMeshes.clear();

            std::error_code ec;
            const auto fileSize = std::filesystem::file_size(cachePath, ec);
            if (ec) {
                return false;
            }

            std::ifstream file(cachePath, std::ios::binary);
            Header header;
            if (!file || !ReadHeader(file, header)) {
                DebugLog(("StaticMeshCache: unusable cache header \"" + cachePath.string() + "\"\n").c_str());
                return false;
            }

            // Every count is checked against the bytes that actually remain before it is
            // used to size a vector: a truncated or hostile file must fail, not allocate.
            const auto remaining = [&]() -> uint64_t {
                const auto pos = file.tellg();
                if (pos < 0 || static_cast<uint64_t>(pos) > fileSize) {
                    return 0;
                }
                return fileSize - static_cast<uint64_t>(pos);
            };

            if (header.submeshCount > remaining() / sizeof(uint32_t)) {
                DebugLog(("StaticMeshCache: submesh count exceeds file size \"" + cachePath.string() + "\"\n").c_str());
                return false;
            }

            outMeshes.reserve(header.submeshCount);
            for (uint32_t i = 0; i < header.submeshCount; ++i) {
                uint32_t vertexCount = 0;
                uint32_t indexCount = 0;
                file.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
                file.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
                if (!file) {
                    outMeshes.clear();
                    return false;
                }

                const uint64_t bytesLeft = remaining();
                const uint64_t needed = static_cast<uint64_t>(vertexCount) * sizeof(Vertex) +
                                        static_cast<uint64_t>(indexCount) * sizeof(uint32_t) +
                                        sizeof(SurfaceMaterial) + sizeof(float) * 16u;
                if (needed > bytesLeft) {
                    DebugLog(("StaticMeshCache: truncated submesh data \"" + cachePath.string() + "\"\n").c_str());
                    outMeshes.clear();
                    return false;
                }

                LoadedStaticMesh mesh;
                mesh.mesh.vertices.resize(vertexCount);
                mesh.mesh.indices.resize(indexCount);
                if (vertexCount > 0) {
                    file.read(reinterpret_cast<char*>(mesh.mesh.vertices.data()),
                              static_cast<std::streamsize>(vertexCount) * sizeof(Vertex));
                }
                if (indexCount > 0) {
                    file.read(reinterpret_cast<char*>(mesh.mesh.indices.data()),
                              static_cast<std::streamsize>(indexCount) * sizeof(uint32_t));
                }
                file.read(reinterpret_cast<char*>(&mesh.material), sizeof(SurfaceMaterial));
                file.read(reinterpret_cast<char*>(mesh.localTransform), sizeof(mesh.localTransform));
                if (!file) {
                    outMeshes.clear();
                    return false;
                }

                if (!ReadString(file, remaining(), mesh.texturePath) ||
                    !ReadString(file, remaining(), mesh.occlusionTexturePath) ||
                    !ReadString(file, remaining(), mesh.metallicRoughnessTexturePath) ||
                    !ReadString(file, remaining(), mesh.normalTexturePath)) {
                    DebugLog(("StaticMeshCache: truncated texture paths \"" + cachePath.string() + "\"\n").c_str());
                    outMeshes.clear();
                    return false;
                }

                outMeshes.push_back(std::move(mesh));
            }

            char message[256];
            snprintf(message, sizeof(message),
                     "[Perf] StaticMeshCache: loaded %zu submeshes from cache (%llu KB)\n",
                     outMeshes.size(), static_cast<unsigned long long>(fileSize / 1024u));
            DebugLog(message);
            return true;
        }

        bool Save(const std::filesystem::path& cachePath,
                  const std::string& sourcePath,
                  float uniformScale,
                  const std::vector<LoadedStaticMesh>& meshes)
        {
            uint64_t sourceSize = 0;
            int64_t sourceWriteTime = 0;
            if (!ReadSourceStamp(sourcePath, sourceSize, sourceWriteTime)) {
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(cachePath.parent_path(), ec);
            if (ec) {
                DebugLog(("StaticMeshCache: cannot create cache directory \"" +
                          cachePath.parent_path().string() + "\"\n").c_str());
                return false;
            }

            // Write to a temporary and rename: an interrupted write must never leave a
            // half-file that later passes validation.
            std::filesystem::path tempPath = cachePath;
            tempPath += ".tmp";
            {
                std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
                if (!file) {
                    DebugLog(("StaticMeshCache: cannot open \"" + tempPath.string() + "\" for writing\n").c_str());
                    return false;
                }

                Header header;
                std::memcpy(header.magic, kMagic, sizeof(kMagic));
                header.formatVersion = kFormatVersion;
                header.sourceFileSize = sourceSize;
                header.sourceWriteTime = sourceWriteTime;
                header.uniformScale = uniformScale;
                header.submeshCount = static_cast<uint32_t>(meshes.size());
                file.write(reinterpret_cast<const char*>(&header), sizeof(Header));

                for (const LoadedStaticMesh& mesh : meshes) {
                    const uint32_t vertexCount = static_cast<uint32_t>(mesh.mesh.vertices.size());
                    const uint32_t indexCount = static_cast<uint32_t>(mesh.mesh.indices.size());
                    file.write(reinterpret_cast<const char*>(&vertexCount), sizeof(vertexCount));
                    file.write(reinterpret_cast<const char*>(&indexCount), sizeof(indexCount));
                    if (vertexCount > 0) {
                        file.write(reinterpret_cast<const char*>(mesh.mesh.vertices.data()),
                                   static_cast<std::streamsize>(vertexCount) * sizeof(Vertex));
                    }
                    if (indexCount > 0) {
                        file.write(reinterpret_cast<const char*>(mesh.mesh.indices.data()),
                                   static_cast<std::streamsize>(indexCount) * sizeof(uint32_t));
                    }
                    file.write(reinterpret_cast<const char*>(&mesh.material), sizeof(SurfaceMaterial));
                    file.write(reinterpret_cast<const char*>(mesh.localTransform), sizeof(mesh.localTransform));
                    WriteString(file, mesh.texturePath);
                    WriteString(file, mesh.occlusionTexturePath);
                    WriteString(file, mesh.metallicRoughnessTexturePath);
                    WriteString(file, mesh.normalTexturePath);
                }

                if (!file) {
                    file.close();
                    std::filesystem::remove(tempPath, ec);
                    DebugLog(("StaticMeshCache: write failed for \"" + tempPath.string() + "\"\n").c_str());
                    return false;
                }
            }

            std::filesystem::remove(cachePath, ec);
            std::filesystem::rename(tempPath, cachePath, ec);
            if (ec) {
                std::error_code cleanupEc;
                std::filesystem::remove(tempPath, cleanupEc);
                DebugLog(("StaticMeshCache: cannot publish \"" + cachePath.string() + "\"\n").c_str());
                return false;
            }

            const auto written = std::filesystem::file_size(cachePath, ec);
            char message[256];
            snprintf(message, sizeof(message),
                     "StaticMeshCache: saved %zu submeshes (%llu KB) -> %s\n",
                     meshes.size(),
                     static_cast<unsigned long long>(ec ? 0u : written / 1024u),
                     cachePath.filename().string().c_str());
            DebugLog(message);
            return true;
        }
    }
}
