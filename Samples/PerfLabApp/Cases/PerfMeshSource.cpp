#include "Cases/PerfMeshSource.h"

#include "ApplicationResourcePaths.h"
#include "Loader/ModelLoader.h"
#include "meshoptimizer/meshoptimizer.h"

#include <cctype>
#include <cstdio>
#include <filesystem>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
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
    }

    bool LoadWeldedMesh(const std::string& assetPath, PerfMeshSource& mesh, std::string& error)
    {
        App::StaticModelFormat format;
        if (!FormatFromPath(assetPath, format)) {
            error = "unknown model format: " + assetPath;
            return false;
        }
        const std::string fullPath = App::ApplicationResourcePaths::ResolveAssetPathString(assetPath);
        std::vector<App::LoadedStaticMesh> meshes;
        if (!App::LoadStaticModel(fullPath, format, 1.0f, meshes) || meshes.empty()) {
            error = "failed to load " + fullPath;
            return false;
        }

        std::vector<Render::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const App::LoadedStaticMesh& m : meshes) {
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            vertices.insert(vertices.end(), m.mesh.vertices.begin(), m.mesh.vertices.end());
            for (uint32_t i : m.mesh.indices) {
                indices.push_back(base + i);
            }
        }
        meshes.clear();
        if (indices.size() < 3 || indices.size() % 3 != 0) {
            error = "model has no triangle list";
            return false;
        }

        // OBJ loads unindexed, so welding duplicates first is what makes an index buffer meaningful.
        constexpr size_t kVertexSize = sizeof(Render::Vertex);
        std::vector<uint32_t> remap(vertices.size());
        const size_t vertexCount = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
                                                               vertices.data(), vertices.size(), kVertexSize);
        mesh.vertices.resize(vertexCount);
        meshopt_remapVertexBuffer(mesh.vertices.data(), vertices.data(), vertices.size(), kVertexSize, remap.data());
        mesh.indices.resize(indices.size());
        meshopt_remapIndexBuffer(mesh.indices.data(), indices.data(), indices.size(), remap.data());
        return true;
    }

    std::string FormatKiB(size_t bytes)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
        return text;
    }
}
