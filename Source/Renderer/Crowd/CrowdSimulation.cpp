#include "Renderer/Crowd/CrowdSimulation.h"

#include "Foundation/Jobs/ParallelFor.h"

#include <algorithm>

namespace SasamiEngine::Render::Crowd
{
    void StepCrowdRange(CrowdStore& store, const CrowdSimParams& params, uint32_t begin, uint32_t end)
    {
        const float dt = params.deltaTime;
        const float minX = params.boundsMinX;
        const float maxX = params.boundsMaxX;
        const float minZ = params.boundsMinZ;
        const float maxZ = params.boundsMaxZ;
        const uint32_t clipCount = (std::max)(params.clipCount, 1u);

        for (uint32_t i = begin; i < end; ++i) {
            float x = store.posX[i];
            float z = store.posZ[i];
            float vx = store.velX[i];
            float vz = store.velZ[i];
            float t = store.animTime[i];
            const float rate = store.animRate[i];
            const uint32_t clipIndex = (std::min)(store.clip[i], clipCount - 1u);

            const float dx = vx * dt;
            x = x + dx;
            if (x < minX) {
                const float over = minX - x;
                x = minX + over;
                vx = -vx;
            } else if (x > maxX) {
                const float over = x - maxX;
                x = maxX - over;
                vx = -vx;
            }

            const float dz = vz * dt;
            z = z + dz;
            if (z < minZ) {
                const float over = minZ - z;
                z = minZ + over;
                vz = -vz;
            } else if (z > maxZ) {
                const float over = z - maxZ;
                z = maxZ - over;
                vz = -vz;
            }

            const float dtr = dt * rate;
            t = t + dtr;
            const float d = params.clipDurations[clipIndex];
            if (t >= d) {
                t = t - d;
            }

            store.posX[i] = x;
            store.posZ[i] = z;
            store.velX[i] = vx;
            store.velZ[i] = vz;
            store.animTime[i] = t;
        }
    }

    void StepCrowdCpu(CrowdStore& store, const CrowdSimParams& params, CrowdUpdateBackend backend)
    {
        const uint32_t count = store.Count();
        if (backend == CrowdUpdateBackend::CpuSerial) {
            StepCrowdRange(store, params, 0, count);
        } else if (backend == CrowdUpdateBackend::CpuParallel) {
            const uint32_t chunkCount = (count + CROWD_UPDATE_CHUNK - 1u) / CROWD_UPDATE_CHUNK;
            Foundation::ParallelFor(chunkCount, [&](uint32_t chunk) {
                const uint32_t begin = chunk * CROWD_UPDATE_CHUNK;
                const uint32_t end = (std::min)(begin + CROWD_UPDATE_CHUNK, count);
                StepCrowdRange(store, params, begin, end);
            });
        }
    }
}
