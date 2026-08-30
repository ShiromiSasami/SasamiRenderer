#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "Foundation/Boot/BootStatus.h"
#include "Foundation/Boot/MainThreadTaskPump.h"
#include "Foundation/Jobs/JobCounter.h"

namespace SasamiRenderer
{
    // Dependency-ordered dispatcher for initialization tasks. Worker-affinity tasks run
    // on the JobSystem pool; main-thread-affinity tasks are handed to a MainThreadTaskPump
    // for time-sliced execution. Completion is observed via each task's atomic state --
    // Pump() never blocks, so the caller's message loop keeps running while init drains.
    //
    // Task storage is a std::deque so records have stable addresses: worker jobs receive
    // a TaskRecord* as their POD Job userdata and the record must outlive its JobCounter
    // (see JobTypes.h). The scheduler must therefore outlive all dispatched work; call
    // WaitForOutstandingWork() before JobSystem::Shutdown().
    class InitTaskScheduler
    {
    public:
        using TaskId = uint32_t;
        static constexpr TaskId kInvalidTask = ~0u;

        enum class TaskState : uint8_t
        {
            Pending,    // waiting on dependencies or dispatch
            Dispatched, // handed to JobSystem / MainThreadTaskPump
            Running,
            Done,
            Failed
        };

        // Worker-affinity task: pure CPU work or thread-safe device work only.
        // name must be a stable string (literal); shown in BootStatus.
        TaskId AddWorkerTask(const char* name,
                             std::function<bool()> work,
                             std::vector<TaskId> dependsOn = {});

        // Main-thread-affinity task with a resumable step (see MainThreadTaskPump).
        TaskId AddMainThreadTask(const char* name,
                                 std::function<InitStepResult()> step,
                                 std::vector<TaskId> dependsOn = {});

        // Convenience: main-thread task that completes in a single step.
        TaskId AddMainThreadTask(const char* name,
                                 std::function<bool()> work,
                                 std::vector<TaskId> dependsOn = {});

        // Dispatches every Pending task whose dependencies are Done. Main thread only,
        // once per frame. Non-blocking. Tasks with a failed dependency become Failed
        // without running.
        void Pump(MainThreadTaskPump& pump);

        bool AllDone() const;   // every task Done or Failed
        bool AnyFailed() const;
        uint32_t CompletedCount() const;
        uint32_t TotalCount() const;

        // Appends one BootStatus item per task (up to BootStatus::kMaxItems).
        void CollectStatus(BootStatus& out) const;

        // Blocks until all Dispatched/Running worker tasks finish. Shutdown-time only
        // (the single permitted wait); main-thread tasks still queued are dropped.
        void WaitForOutstandingWork();

    private:
        struct TaskRecord
        {
            const char* name = "";
            std::function<bool()> work;                    // worker affinity
            std::function<InitStepResult()> step;          // main-thread affinity
            std::vector<TaskId> dependsOn;
            bool mainThread = false;
            std::atomic<TaskState> state{TaskState::Pending};
            JobCounter counter;
            std::atomic<float> elapsedMs{0.0f};
        };

        static void RunWorkerTaskJob(void* userdata);
        bool DependenciesDone(const TaskRecord& record) const;
        bool AnyDependencyFailed(const TaskRecord& record) const;

        std::deque<TaskRecord> m_tasks;
    };
}
