#include "AppFramework/Debug/ParticleCharacterModel.h"

#include "AppFramework/ApplicationResourcePaths.h"
#include "Foundation/Tools/ScopedPerfTimer.h"

#include <chrono>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        using Foundation::MillisecondsSince;
    }

    const char* ParticleCharacterModelPath(const std::string& model)
    {
        if (model == "fox") {
            return "Models/Fox/Fox.gltf";
        }
        if (model == "vroid") {
            return "Models/VRoidAvatarSampleD/AvatarSample_D.glb";
        }
        return nullptr;
    }

    bool LoadParticleCharacterModel(const std::string& model, uint32_t count, uint32_t seed, uint32_t clip,
                                    ParticleCharacterModel& out, std::string& error)
    {
        const char* relative = ParticleCharacterModelPath(model);
        if (!relative) {
            error = "unknown model " + model;
            return false;
        }
        const std::string path = ApplicationResourcePaths::ResolveAssetPathString(relative);
        if (!LoadGLTFSkinned(path, out.data)) {
            error = "load failed " + path;
            return false;
        }

        auto start = std::chrono::steady_clock::now();
        std::string sampleError;
        if (!Render::ParticleCharacter::SampleSkinnedSurface(out.data.meshes, count, seed, out.set, sampleError)) {
            error = "sample: " + sampleError;
            return false;
        }
        out.sampleMs = MillisecondsSince(start);

        Render::Crowd::VatBakeDesc desc;
        desc.mode = Render::Crowd::VatMode::Vertex;
        desc.sampleRate = 30.0f;
        desc.clipIndices = { static_cast<int>(clip) };
        start = std::chrono::steady_clock::now();
        std::string bakeError;
        if (!Render::Crowd::BakeVat(out.data.meshes, out.data.skeleton, out.data.animations, desc, out.baked, bakeError)) {
            error = "bake: " + bakeError;
            return false;
        }
        out.bakeMs = MillisecondsSince(start);
        if (out.baked.clips.empty() || out.baked.vertexCount != out.set.vertexCount) {
            error = "bake: vertex count mismatch";
            return false;
        }
        out.clipDuration = out.baked.clips[0].durationSec;
        return true;
    }

    bool EvaluateParticleCharacterPose(const ParticleCharacterModel& model, float t,
                                       std::vector<float>& positions, std::vector<float>& normals)
    {
        positions.resize(static_cast<size_t>(model.baked.vertexCount) * 3);
        normals.resize(static_cast<size_t>(model.baked.vertexCount) * 3);
        uint32_t vertexIndex = 0;
        for (const Render::SkinnedMesh& mesh : model.data.meshes) {
            for (const Render::SkinnedVertex& vertex : mesh.vertices) {
                if (vertexIndex >= model.baked.vertexCount
                    || !Render::Crowd::SampleVatVertex(model.baked, 0, t, vertexIndex, vertex,
                                                       &positions[static_cast<size_t>(vertexIndex) * 3],
                                                       &normals[static_cast<size_t>(vertexIndex) * 3])) {
                    return false;
                }
                ++vertexIndex;
            }
        }
        return vertexIndex == model.baked.vertexCount;
    }
}
