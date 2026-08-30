#include "Foundation/Jobs/JobSystem.h"

#define NOMINMAX
#include "Foundation/Jobs/JobCounter.h"
#include "Foundation/Jobs/JobWorker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <semaphore>
#include <thread>

#if defined(_DEBUG)
#include "Foundation/Jobs/ParallelFor.h"
#include "Foundation/Tools/DebugOutput.h"

#include <cstdio>
#include <vector>
#endif

namespace SasamiRenderer::JobSystem
{
    namespace
    {
        // Power of two. Sized well above the largest single fan-out (per-mesh BVH builds
        // over a streamed scene reach a few thousand) so the wrapping slot counter cannot
        // lap a job that is still queued. Workers copy their descriptor before running it,
        // which covers slots recycled during execution; this headroom covers the queue.
        constexpr size_t kJobPoolCapacity = 65536;
        constexpr uint32_t kMaxWorkers = 64;

        std::array<Job, kJobPoolCapacity> g_jobPool;
        std::atomic<uint64_t> g_nextJobIndex{ 0 };

        std::array<JobWorker, kMaxWorkers> g_workers;
        std::counting_semaphore<> g_jobsAvailable{ 0 };

        uint32_t g_workerCount = 0;
        std::atomic<uint32_t> g_nextWorkerForKick{ 0 };

        // Reserves one slot in the fixed-size job pool and copies job into it.
        // Slots are recycled as g_nextJobIndex wraps. Executing jobs are safe regardless
        // (JobWorker copies the descriptor before invoking it); the pool is sized so a
        // wrap cannot reach a job that is still sitting in a worker queue.
        Job* ReserveJobSlot(const Job& job)
        {
            const uint64_t slot = g_nextJobIndex.fetch_add(1, std::memory_order_relaxed) & (kJobPoolCapacity - 1);
            Job& slotJob = g_jobPool[slot];
            slotJob = job;
            return &slotJob;
        }

        void PushToAnyWorker(Job* job)
        {
            bool pushed = false;
            for (uint32_t attempt = 0; attempt < g_workerCount && !pushed; ++attempt) {
                const uint32_t target = g_nextWorkerForKick.fetch_add(1, std::memory_order_relaxed) % g_workerCount;
                pushed = g_workers[target].TryPush(job);
            }
        }
    }

    void Initialize(uint32_t workerCount)
    {
        if (workerCount == 0) {
            workerCount = std::max(1u, std::thread::hardware_concurrency() - 1);
        }
        workerCount = std::min(workerCount, kMaxWorkers);

        g_workerCount = workerCount;
        for (uint32_t i = 0; i < g_workerCount; ++i) {
            g_workers[i].Start(i, g_workerCount, g_workers.data(), &g_jobsAvailable);
        }
    }

    void Shutdown()
    {
        // Request stop on every worker before releasing the shared semaphore,
        // so a worker parked in g_jobsAvailable.acquire() wakes up with its
        // own stop already requested rather than Stop() joining a thread
        // that's still blocked waiting to be woken.
        for (uint32_t i = 0; i < g_workerCount; ++i) {
            g_workers[i].RequestStop();
        }

        g_jobsAvailable.release(static_cast<std::ptrdiff_t>(g_workerCount));

        for (uint32_t i = 0; i < g_workerCount; ++i) {
            g_workers[i].Stop();
        }

        g_workerCount = 0;
    }

    void Kick(JobFunction fn, void* userdata, JobCounter* counter)
    {
        Job* job = ReserveJobSlot(Job{ fn, userdata, counter });
        PushToAnyWorker(job);
        g_jobsAvailable.release();
    }

    void KickN(const Job* jobs, uint32_t count, JobCounter* counter)
    {
        for (uint32_t i = 0; i < count; ++i) {
            Job* job = ReserveJobSlot(jobs[i]);
            if (counter) {
                job->counter = counter;
            }
            PushToAnyWorker(job);
        }

        g_jobsAvailable.release(static_cast<std::ptrdiff_t>(count));
    }

    void Wait(const JobCounter& counter)
    {
        counter.Wait();
    }

    void KickAndWait(const Job* jobs, uint32_t count)
    {
        JobCounter counter;
        counter.Reset(static_cast<int32_t>(count));
        KickN(jobs, count, &counter);
        Wait(counter);
    }

    uint32_t GetWorkerCount()
    {
        return g_workerCount;
    }

#if defined(_DEBUG)
    namespace
    {
        void IncrementCounterJob(void* userdata)
        {
            reinterpret_cast<std::atomic<int>*>(userdata)->fetch_add(1, std::memory_order_relaxed);
        }
    }

    void RunSelfTest()
    {
        {
            constexpr size_t kJobCount = 10000;
            std::atomic<int> counter{ 0 };
            std::vector<Job> jobs(kJobCount);
            for (auto& job : jobs) {
                job.function = IncrementCounterJob;
                job.userdata = &counter;
            }

            KickAndWait(jobs.data(), static_cast<uint32_t>(jobs.size()));

            const int result = counter.load(std::memory_order_relaxed);
            const bool pass = (result == static_cast<int>(kJobCount));

            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                "[JobSystemSelfTest] FanOutFanIn: %s (got %d, expected %zu)\n",
                pass ? "PASS" : "FAIL", result, kJobCount);
            DebugLog(buffer);
        }

        {
            std::vector<int> results(4096, 0);
            ParallelFor(static_cast<uint32_t>(results.size()), [&results](uint32_t i) {
                results[i] = static_cast<int>(i) * 2;
            });

            bool pass = true;
            for (size_t i = 0; i < results.size(); ++i) {
                if (results[i] != static_cast<int>(i) * 2) {
                    pass = false;
                    break;
                }
            }

            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                "[JobSystemSelfTest] ParallelFor: %s\n",
                pass ? "PASS" : "FAIL");
            DebugLog(buffer);
        }
    }
#endif
}
