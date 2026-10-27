#pragma once

#include "AppFramework/Loader/ModelLoader.h"
#include "Renderer/Crowd/VatAnimation.h"
#include "Renderer/ParticleCharacter/SurfaceSampleSet.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    // Asset path (relative to Assets/) of a particle-character test model: "fox" or "vroid"; null otherwise.
    const char* ParticleCharacterModelPath(const std::string& model);

    // A skinned model, its surface samples and one Vertex-mode VAT clip, shared by pchar.bake / pchar.sim
    // and the pchar.view overlay.
    struct ParticleCharacterModel
    {
        SkinnedModelData data;
        Render::Crowd::VatBakedAnimation baked;
        Render::ParticleCharacter::SurfaceSampleSet set;
        double sampleMs = 0.0;
        double bakeMs = 0.0;
        float clipDuration = 0.0f;
    };

    bool LoadParticleCharacterModel(const std::string& model, uint32_t count, uint32_t seed, uint32_t clip,
                                    ParticleCharacterModel& out, std::string& error);

    // Packed stride-3 position / normal of every concatenated vertex at playback time t of the baked clip
    // (SampleVatVertex wraps t over the clip).
    bool EvaluateParticleCharacterPose(const ParticleCharacterModel& model, float t,
                                       std::vector<float>& positions, std::vector<float>& normals);
}

namespace SasamiEngine { using namespace App; }
