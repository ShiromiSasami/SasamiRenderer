#pragma once

#include "Foundation/Jobs/JobTypes.h"
#include "Foundation/Jobs/WorkStealingDeque.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <semaphore>
#include <thread>

namespace SasamiRenderer
{
    // Must be large enough that worker queue capacity (this * worker count)
    // comfortably exceeds the largest single KickN batch, since WorkerLoop now
    // waits for the whole batch's semaphore release before consuming any of it
    // -- the entire batch sits queued at once rather than draining as it's
    // pushed. Matches kJobPoolCapacity in JobSystem.cpp for the same reason.
    constexpr size_t kJobWorkerQueueCapacity = 4096; // power of two

    // Owns one OS worker thread and its own work-stealing deque. Runs:
    // pop own queue -> steal from a sibling -> block on the shared semaphore
    // when idle. Lifetime is Start()/Stop(); Stop() requests std::jthread
    // cooperative cancellation and joins (via the jthread destructor).
    class JobWorker
    {
    public:
        JobWorker() = default;
        JobWorker(const JobWorker&) = delete;
        JobWorker& operator=(const JobWorker&) = delete;

        // Starts the worker thread. workerIndex/workerCount identify this worker
        // among its siblings for steal-target selection. siblings points at the
        // full worker array (owned by JobSystem, must outlive this JobWorker and
        // have workerCount elements). jobsAvailable is the semaphore JobSystem
        // releases on Kick/KickN and idle workers acquire from while waiting.
        void Start(uint32_t workerIndex, uint32_t workerCount, JobWorker* siblings,
                   std::counting_semaphore<>* jobsAvailable);

        // Requests the worker thread to stop without joining. Callers that
        // stop multiple workers sharing one semaphore must call this on all
        // of them, then release the semaphore enough times to wake any that
        // are idle-blocked, before calling Stop() -- otherwise Stop() can
        // join a worker that's parked in the semaphore forever.
        void RequestStop();

        // Requests the worker thread to stop and joins it.
        void Stop();

        // Pushes a job onto this worker's own queue. Safe to call from any
        // thread (JobSystem::Kick/KickN does the round-robin distribution).
        // Returns false if this worker's queue is full; the caller should
        // route the job to another worker.
        bool TryPush(Job* job);

        // Attempts to steal one job from this worker's queue. Called by
        // sibling JobWorker instances when their own queue is empty.
        std::optional<Job*> TrySteal();

    private:
        void WorkerLoop(std::stop_token stopToken);
        std::optional<Job*> TryGetJob();
        static void RunJob(const Job& job);

        WorkStealingDeque<Job*, kJobWorkerQueueCapacity> m_queue;
        std::jthread m_thread;
        uint32_t m_workerIndex = 0;
        uint32_t m_workerCount = 0;
        JobWorker* m_siblings = nullptr;
        std::counting_semaphore<>* m_jobsAvailable = nullptr;
    };
}
