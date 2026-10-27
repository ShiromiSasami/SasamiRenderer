#include "Renderer/Crowd/CrowdStore.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        // SplitMix32-style finalizer over (seed, index, stream).
        uint32_t HashU32(uint32_t seed, uint32_t index, uint32_t stream)
        {
            uint32_t z = seed + 0x9E3779B9u * (index + 1u) + 0x85EBCA6Bu * (stream + 1u);
            z = (z ^ (z >> 16)) * 0x21F0AAADu;
            z = (z ^ (z >> 15)) * 0x735A2D97u;
            return z ^ (z >> 15);
        }

        float Unit(uint32_t h)
        {
            return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
        }
    }

    void CrowdStore::Resize(uint32_t count)
    {
        posX.resize(count);
        posY.resize(count);
        posZ.resize(count);
        velX.resize(count);
        velZ.resize(count);
        animTime.resize(count);
        animRate.resize(count);
        clip.resize(count);
    }

    void SpawnCrowd(CrowdStore& store, uint32_t count, const CrowdSimParams& params,
                    float minSpeed, float maxSpeed, uint32_t seed)
    {
        store.Resize(count);
        if (count == 0) {
            return;
        }

        const uint32_t clipCount = (std::max)(params.clipCount, 1u);
        const uint32_t cols = (std::max)(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(count)))));
        const uint32_t rows = (count + cols - 1u) / cols;
        const float width = params.boundsMaxX - params.boundsMinX;
        const float depth = params.boundsMaxZ - params.boundsMinZ;
        const float cellX = width / static_cast<float>(cols);
        const float cellZ = depth / static_cast<float>(rows);
        const float TWO_PI = 6.28318530717958647692f;

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t col = i % cols;
            const uint32_t row = i / cols;
            const float jx = (Unit(HashU32(seed, i, 0)) * 2.0f - 1.0f) * 0.4f;
            const float jz = (Unit(HashU32(seed, i, 1)) * 2.0f - 1.0f) * 0.4f;
            const float x = params.boundsMinX + (static_cast<float>(col) + 0.5f + jx) * cellX;
            const float z = params.boundsMinZ + (static_cast<float>(row) + 0.5f + jz) * cellZ;
            store.posX[i] = (std::min)((std::max)(x, params.boundsMinX), params.boundsMaxX);
            store.posY[i] = 0.0f;
            store.posZ[i] = (std::min)((std::max)(z, params.boundsMinZ), params.boundsMaxZ);

            const float angle = Unit(HashU32(seed, i, 2)) * TWO_PI;
            const float speed = minSpeed + (maxSpeed - minSpeed) * Unit(HashU32(seed, i, 3));
            store.velX[i] = speed * std::cos(angle);
            store.velZ[i] = speed * std::sin(angle);

            const uint32_t clipIndex = HashU32(seed, i, 4) % clipCount;
            store.clip[i] = clipIndex;
            store.animTime[i] = Unit(HashU32(seed, i, 5)) * params.clipDurations[(std::min)(clipIndex, CROWD_MAX_CLIPS - 1u)];
            store.animRate[i] = 0.8f + 0.4f * Unit(HashU32(seed, i, 6));
        }
    }
}
