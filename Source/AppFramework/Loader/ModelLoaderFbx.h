#pragma once

#include <string>
#include <vector>

#include "Loader/ModelLoader.h"

namespace SasamiRenderer
{
    // FBX loading via the vendored ufbx library (Libraries/ufbx). Produces the same
    // LoadedStaticMesh list as the glTF/OBJ paths, so everything downstream
    // (MeshComponent, async loading, render proxies) is format-agnostic.
    //
    // CPU-only: parses geometry and records texture paths, never touches the GPU or
    // decodes images, so it is safe to call from a JobSystem worker.
    bool LoadStaticModelFbx(const std::string& path,
                            float uniformScale,
                            std::vector<LoadedStaticMesh>& outMeshes);
}
