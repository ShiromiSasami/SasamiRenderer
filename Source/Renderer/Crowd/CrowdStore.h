#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Crowd
{
    // Data-oriented crowd state: one array per field (structure of arrays), so an update pass
    // touches only the fields it reads and writes, contiguously. Agents live on the XZ ground
    // plane; posY is carried for the renderer but is not integrated.
    struct CrowdStore
    {
        std::vector<float> posX;
        std::vector<float> posY;
        std::vector<float> posZ;
        std::vector<float> velX;
        std::vector<float> velZ;
        std::vector<float> animTime;  // seconds into the agent's clip, kept in [0, clip duration)
        std::vector<float> animRate;  // playback speed multiplier
        std::vector<uint32_t> clip;   // index into CrowdSimParams::clipDurations

        uint32_t Count() const { return static_cast<uint32_t>(posX.size()); }
        void Resize(uint32_t count);
    };

    static constexpr uint32_t CROWD_MAX_CLIPS = 8;

    struct CrowdSimParams
    {
        float boundsMinX = -50.0f;
        float boundsMinZ = -50.0f;
        float boundsMaxX = 50.0f;
        float boundsMaxZ = 50.0f;
        float deltaTime = 1.0f / 60.0f;
        uint32_t clipCount = 1;
        float clipDurations[CROWD_MAX_CLIPS] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    };

    // Fills `store` with `count` agents on a jittered grid inside the params bounds, with
    // random headings, speeds in [minSpeed, maxSpeed], random clip / start time / rate.
    // Deterministic for a given seed (integer hash, no std:: distributions).
    void SpawnCrowd(CrowdStore& store, uint32_t count, const CrowdSimParams& params,
                    float minSpeed, float maxSpeed, uint32_t seed);
}

namespace SasamiEngine { using namespace Render; }
