#include "ModelLoaderEmbeddedImages.h"

#include "Foundation/Tools/DebugOutput.h"

#include <windows.h>

#include <fstream>

namespace SasamiEngine::App
{
    namespace {
        std::filesystem::path GetEmbeddedImageDirectory()
        {
            wchar_t buffer[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (length == 0) {
                return std::filesystem::path(L"AssetCache") / L"EmbeddedImages";
            }
            return std::filesystem::path(buffer).parent_path() / L"AssetCache" / L"EmbeddedImages";
        }

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

        const char* ExtensionForMimeType(const std::string& mimeType)
        {
            if (mimeType == "image/jpeg") return ".jpg";
            if (mimeType == "image/png") return ".png";
            return nullptr; // KTX2 / WebP need extensions the loaders do not handle.
        }

        std::string WriteEmbeddedImage(const std::filesystem::path& sourcePath, rapidjson::SizeType imageIndex,
                                       const uint8_t* data, size_t size, const char* extension)
        {
            const std::filesystem::path directory = GetEmbeddedImageDirectory();
            const std::string name = SanitizeForFilename(sourcePath.filename().string()) + "_" +
                                     std::to_string(imageIndex) + extension;
            const std::filesystem::path target = directory / std::filesystem::path(name);

            std::error_code ec;
            if (std::filesystem::exists(target, ec) && std::filesystem::file_size(target, ec) == size && !ec) {
                return target.string();
            }

            std::filesystem::create_directories(directory, ec);
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            if (!file) {
                DebugLog(("ModelLoader: could not write embedded image " + target.string() + "\n").c_str());
                return std::string();
            }
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            return file ? target.string() : std::string();
        }
    }

    std::vector<std::string> ResolveGltfImagePaths(const rapidjson::Document& doc,
                                                   const std::filesystem::path& sourcePath,
                                                   const std::vector<std::vector<uint8_t>>& buffers,
                                                   const std::vector<ModelLoaderGltfUtility::BufferView>& views)
    {
        std::vector<std::string> paths;
        if (!doc.HasMember("images") || !doc["images"].IsArray()) {
            return paths;
        }

        const std::filesystem::path basePath = sourcePath.parent_path();
        const auto& images = doc["images"];
        paths.resize(images.Size());
        for (rapidjson::SizeType i = 0; i < images.Size(); ++i) {
            const auto& image = images[i];
            if (image.HasMember("uri") && image["uri"].IsString()) {
                const std::string uri = image["uri"].GetString();
                if (uri.rfind("data:", 0) != 0) {
                    paths[i] = (basePath / uri).string();
                }
                continue;
            }
            if (!image.HasMember("bufferView") || !image["bufferView"].IsInt()) {
                continue;
            }

            const int viewIndex = image["bufferView"].GetInt();
            const std::string mimeType = (image.HasMember("mimeType") && image["mimeType"].IsString())
                                             ? image["mimeType"].GetString()
                                             : std::string();
            const char* extension = ExtensionForMimeType(mimeType);
            if (viewIndex < 0 || static_cast<size_t>(viewIndex) >= views.size() || !extension) {
                continue;
            }
            const auto& view = views[static_cast<size_t>(viewIndex)];
            if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= buffers.size()) {
                continue;
            }
            const auto& buffer = buffers[static_cast<size_t>(view.buffer)];
            if (view.byteOffset + view.byteLength > buffer.size() || view.byteLength == 0) {
                continue;
            }
            paths[i] = WriteEmbeddedImage(sourcePath, i, buffer.data() + view.byteOffset, view.byteLength, extension);
        }
        return paths;
    }
}
