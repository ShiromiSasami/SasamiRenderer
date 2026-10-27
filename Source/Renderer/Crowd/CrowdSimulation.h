#pragma once

#include "Renderer/Crowd/CrowdStore.h"

namespace SasamiEngine::Render::Crowd
{
    enum class CrowdUpdateBackend
    {
        CpuSerial,    // single-threaded reference
        CpuParallel,  // chunked Foundation::ParallelFor; bit-identical to CpuSerial
        Gpu           // GpuCrowdUpdater (compute); DX12 only for now
    };

    // Agents per ParallelFor job. One job per agent would drown 100k agents in job overhead.
    static constexpr uint32_t CROWD_UPDATE_CHUNK = 4096;

    // Advances agents [begin, end) by one params.deltaTime step. Per agent, in this exact order
    // (the GPU shader CrowdUpdate_CS.hlsl repeats it operation for operation, so results match
    // bit for bit as long as neither side contracts mul+add into FMA):
    //   x += vx * dt; if (x < minX) { x = minX + (minX - x); vx = -vx; }
    //                 else if (x > maxX) { x = maxX - (x - maxX); vx = -vx; }
    //   same for z / vz
    //   t += dt * rate; d = clipDurations[min(clip, clipCount - 1)]; if (t >= d) t -= d;
    // Mul, add and compare only: no transcendental functions, which differ between CPU and GPU.
    void StepCrowdRange(CrowdStore& store, const CrowdSimParams& params, uint32_t begin, uint32_t end);

    // One step over every agent. backend must be CpuSerial or CpuParallel.
    void StepCrowdCpu(CrowdStore& store, const CrowdSimParams& params, CrowdUpdateBackend backend);
}

namespace SasamiEngine { using namespace Render; }
