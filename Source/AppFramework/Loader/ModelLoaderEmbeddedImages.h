#pragma once

#include "ModelLoaderGltfUtility.h"

#include <filesystem>
#include <string>
#include <vector>

#include <rapidjson/document.h>

namespace SasamiEngine::App
{
    // Resolves every glTF "images" entry to a file path the texture cache can open.
    // - "uri" images resolve relative to basePath, as before. data: URIs are not supported (empty path).
    // - "bufferView" images (GLB / VRM embed their PNGs and JPEGs this way, glTF 2.0 spec 3.6.1) are
    //   written once to <exe>/AssetCache/EmbeddedImages/<source>_<index>.<ext>, because the texture
    //   loaders (WIC / DDS) only take paths. A file of the same size is reused as is.
    // The result has one entry per image; unresolvable images get an empty path.
    std::vector<std::string> ResolveGltfImagePaths(const rapidjson::Document& doc,
                                                   const std::filesystem::path& sourcePath,
                                                   const std::vector<std::vector<uint8_t>>& buffers,
                                                   const std::vector<ModelLoaderGltfUtility::BufferView>& views);
}
namespace SasamiEngine { using namespace App; }
