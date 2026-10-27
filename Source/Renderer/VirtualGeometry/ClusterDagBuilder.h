#pragma once
#include "Renderer/VirtualGeometry/ClusterDag.h"

namespace SasamiEngine::Render
{
    struct Mesh;

    // Builds the cluster LOD DAG of one static mesh (CPU only, deterministic, thread-safe:
    // may be called concurrently for different meshes).
    class ClusterDagBuilder
    {
    public:
        static ClusterDag Build(const Mesh& mesh, const ClusterDagBuildSettings& settings);
    };
}
namespace SasamiEngine { using namespace Render; }
