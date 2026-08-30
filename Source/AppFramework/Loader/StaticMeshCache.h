#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Loader/ModelLoader.h"

namespace SasamiRenderer
{
    // Binary cache of already-parsed, already-converted static meshes.
    //
    // Parsing and converting a large scene dominates startup (Bistro: ~1.9 s of FBX
    // parsing plus ~7.2 s of triangulation/dedup/instancing). The result is a plain
    // vector<LoadedStaticMesh>, so caching it at that level turns every later run into a
    // file read -- and because every format normalizes to this type, glTF and OBJ benefit
    // too. Same idea as the .cso shader cache in ShaderCompilationService.
    //
    // Pure CPU + file IO, no engine state: safe to call from a JobSystem worker.
    namespace StaticMeshCache
    {
        // <exe>/AssetCache/<source path with separators flattened>.<scale>.smesh
        std::filesystem::path ResolveCachePath(const std::string& sourcePath, float uniformScale);

        // True when the cache matches the source file's current size/write time, the same
        // uniformScale, and the current format version. Any mismatch means "re-parse".
        bool IsUpToDate(const std::filesystem::path& cachePath,
                        const std::string& sourcePath,
                        float uniformScale);

        // Returns false on any malformed/truncated file; callers fall back to parsing.
        bool Load(const std::filesystem::path& cachePath, std::vector<LoadedStaticMesh>& outMeshes);

        // Best-effort: a failure is logged and ignored, since the cache is an optimization
        // and never a correctness requirement.
        bool Save(const std::filesystem::path& cachePath,
                  const std::string& sourcePath,
                  float uniformScale,
                  const std::vector<LoadedStaticMesh>& meshes);
    }
}
