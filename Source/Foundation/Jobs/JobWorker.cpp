#include "Foundation/Jobs/JobWorker.h"

#include "Foundation/Jobs/JobCounter.h"

#include <windows.h>

namespace SasamiRenderer
{
    void JobWorker::Start(uint32_t workerIndex, uint32_t workerCount, JobWorker* siblings,
                           std::counting_semaphore<>* jobsAvailable)
    {
        m_workerIndex = workerIndex;
        m_workerCount = workerCount;
        m_siblings = siblings;
        m_jobsAvailable = jobsAvailable;
        m_thread = std::jthread([this](std::stop_token stopToken) { WorkerLoop(stopToken); });
        // Background work (asset decode, BVH builds) must never starve the main/render
        // thread's message pump: with hardware_concurrency-1 workers a wide ParallelFor
        // saturates every core, and at equal priority Windows flags the app "not
        // responding" during multi-second builds. Below-normal keeps workers using idle
        // cores only whenever the main thread is runnable.
        SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
    }

    void JobWorker::RequestStop()
    {
        m_thread.request_stop();
    }

    void JobWorker::Stop()
    {
        // Assigning a fresh jthread requests stop and joins the old one first,
        // so Stop() blocks until the worker thread has actually exited.
        m_thread.request_stop();
        m_thread = std::jthread{};
    }

    bool JobWorker::TryPush(Job* job)
    {
        return m_queue.PushBottom(job);
    }

    std::optional<Job*> JobWorker::TrySteal()
    {
        return m_queue.Steal();
    }

    void JobWorker::WorkerLoop(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested()) {
            // Block until the kicking thread signals that a job is available.
            // This must happen *before* TryGetJob() (not just as an idle-sleep
            // fallback) -- JobSystem::Kick/KickN only release() once all of a
            // batch's ReserveJobSlot writes into the shared job pool are done,
            // so gating consumption on the semaphore guarantees no worker reads
            // a pool slot the kicking thread is still writing.
            m_jobsAvailable->acquire();
            if (stopToken.stop_requested()) {
                break;
            }

            // A permit means one job is available somewhere in the system, but
            // this worker may lose the race for it to a sibling that woke on
            // the same signal; keep scanning until it finds the job its permit
            // represents so permits and executed jobs stay 1:1.
            std::optional<Job*> job;
            do {
                job = TryGetJob();
            } while (!job.has_value() && !stopToken.stop_requested());

            if (job.has_value()) {
                // Copy the descriptor out of the pool before running it. Slots are
                // recycled by a wrapping counter with no in-flight check, so a
                // long-running job (asset loads take tens of seconds) would otherwise
                // see its own function/userdata/counter overwritten mid-execution once
                // enough later jobs wrapped the pool around.
                const Job localJob = *job.value();
                RunJob(localJob);
            }
        }
    }

    std::optional<Job*> JobWorker::TryGetJob()
    {
        std::optional<Job*> job = m_queue.PopBottom();
        if (job.has_value()) {
            return job;
        }

        for (uint32_t i = 1; i < m_workerCount; ++i) {
            const uint32_t siblingIndex = (m_workerIndex + i) % m_workerCount;
            job = m_siblings[siblingIndex].TrySteal();
            if (job.has_value()) {
                return job;
            }
        }

        return std::nullopt;
    }

    void JobWorker::RunJob(const Job& job)
    {
        job.function(job.userdata);
        if (job.counter != nullptr) {
            job.counter->Decrement();
        }
    }
}
