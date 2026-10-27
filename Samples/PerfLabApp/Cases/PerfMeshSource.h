#pragma once

#include "Renderer/Structures/Vertex.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PerfLab
{
    // One triangle list made from every mesh of a model, with duplicate vertices welded
    // (meshopt_generateVertexRemap). Triangle and vertex order stay as the loader produced them.
    struct PerfMeshSource
    {
        std::vector<Render::Vertex> vertices;
        std::vector<uint32_t> indices;
    };

    // assetPath is relative to Assets/ (.obj, .gltf/.glb, .fbx). Mesh transforms are ignored.
    bool LoadWeldedMesh(const std::string& assetPath, PerfMeshSource& mesh, std::string& error);

    std::string FormatKiB(size_t bytes);
}
