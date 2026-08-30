#pragma once

#include "Foundation/Jobs/JobTypes.h"

#include <cstdint>

namespace SasamiRenderer
{
    class JobCounter;
}

namespace SasamiRenderer::JobSystem
{
    // workerCount == 0 selects std::max(1u, std::thread::hardware_concurrency() - 1).
    void Initialize(uint32_t workerCount = 0);
    void Shutdown();

    void Kick(JobFunction fn, void* userdata, JobCounter* counter = nullptr);
    void KickN(const Job* jobs, uint32_t count, JobCounter* counter = nullptr);
    void Wait(const JobCounter& counter);
    void KickAndWait(const Job* jobs, uint32_t count);

    uint32_t GetWorkerCount();

#if defined(_DEBUG)
    // Kicks and waits on a battery of self-checks covering fan-out/fan-in and
    // ParallelFor correctness; logs pass/fail via DebugLog. Call once after
    // Initialize(). Debug-only; compiled out entirely in release.
    void RunSelfTest();
#endif
}
