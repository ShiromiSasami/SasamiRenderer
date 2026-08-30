#include "Foundation/Boot/InitTaskScheduler.h"

#include "Foundation/Jobs/JobSystem.h"

#include <chrono>
#include <cstring>

namespace SasamiRenderer
{
    namespace
    {
        BootItemState ToBootItemState(InitTaskScheduler::TaskState state)
        {
            switch (state) {
            case InitTaskScheduler::TaskState::Pending:
            case InitTaskScheduler::TaskState::Dispatched:
                return BootItemState::Pending;
            case InitTaskScheduler::TaskState::Running:
                return BootItemState::Running;
            case InitTaskScheduler::TaskState::Done:
                return BootItemState::Done;
            case InitTaskScheduler::TaskState::Failed:
            default:
                return BootItemState::Failed;
            }
        }
    }

    InitTaskScheduler::TaskId InitTaskScheduler::AddWorkerTask(const char* name,
                                                                std::function<bool()> work,
                                                                std::vector<TaskId> dependsOn)
    {
        const TaskId id = static_cast<TaskId>(m_tasks.size());
        TaskRecord& record = m_tasks.emplace_back();
        record.name = name;
        record.work = std::move(work);
        record.dependsOn = std::move(dependsOn);
        record.mainThread = false;
        return id;
    }

    InitTaskScheduler::TaskId InitTaskScheduler::AddMainThreadTask(const char* name,
                                                                    std::function<InitStepResult()> step,
                                                                    std::vector<TaskId> dependsOn)
    {
        const TaskId id = static_cast<TaskId>(m_tasks.size());
        TaskRecord& record = m_tasks.emplace_back();
        record.name = name;
        record.step = std::move(step);
        record.dependsOn = std::move(dependsOn);
        record.mainThread = true;
        return id;
    }

    InitTaskScheduler::TaskId InitTaskScheduler::AddMainThreadTask(const char* name,
                                                                    std::function<bool()> work,
                                                                    std::vector<TaskId> dependsOn)
    {
        return AddMainThreadTask(name, [work = std::move(work)]() {
            return work() ? InitStepResult::Done : InitStepResult::Failed;
        }, std::move(dependsOn));
    }

    void InitTaskScheduler::RunWorkerTaskJob(void* userdata)
    {
        TaskRecord* record = static_cast<TaskRecord*>(userdata);

        record->state.store(TaskState::Running, std::memory_order_relaxed);

        const auto startTime = std::chrono::steady_clock::now();
        const bool succeeded = record->work ? record->work() : false;
        const auto endTime = std::chrono::steady_clock::now();

        const double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        record->elapsedMs.store(static_cast<float>(elapsedMs), std::memory_order_relaxed);

        record->state.store(succeeded ? TaskState::Done : TaskState::Failed, std::memory_order_release);
    }

    bool InitTaskScheduler::DependenciesDone(const TaskRecord& record) const
    {
        for (const TaskId dep : record.dependsOn) {
            if (dep >= m_tasks.size()) {
                continue;
            }
            if (m_tasks[dep].state.load(std::memory_order_acquire) != TaskState::Done) {
                return false;
            }
        }
        return true;
    }

    bool InitTaskScheduler::AnyDependencyFailed(const TaskRecord& record) const
    {
        for (const TaskId dep : record.dependsOn) {
            if (dep >= m_tasks.size()) {
                continue;
            }
            if (m_tasks[dep].state.load(std::memory_order_acquire) == TaskState::Failed) {
                return true;
            }
        }
        return false;
    }

    void InitTaskScheduler::Pump(MainThreadTaskPump& pump)
    {
        for (TaskRecord& record : m_tasks) {
            if (record.state.load(std::memory_order_acquire) != TaskState::Pending) {
                continue;
            }

            if (AnyDependencyFailed(record)) {
                record.state.store(TaskState::Failed, std::memory_order_release);
                continue;
            }

            if (!DependenciesDone(record)) {
                continue;
            }

            record.state.store(TaskState::Dispatched, std::memory_order_release);

            if (!record.mainThread) {
                record.counter.Reset(1);
                JobSystem::Kick(&RunWorkerTaskJob, &record, &record.counter);
            } else {
                TaskRecord* recordPtr = &record;
                pump.Enqueue(
                    [recordPtr, startTime = std::chrono::steady_clock::time_point{}]() mutable -> InitStepResult {
                        if (recordPtr->state.load(std::memory_order_acquire) == TaskState::Dispatched) {
                            recordPtr->state.store(TaskState::Running, std::memory_order_relaxed);
                            startTime = std::chrono::steady_clock::now();
                        }

                        const InitStepResult result = recordPtr->step ? recordPtr->step() : InitStepResult::Failed;

                        const auto now = std::chrono::steady_clock::now();
                        const double elapsedMs = std::chrono::duration<double, std::milli>(now - startTime).count();
                        recordPtr->elapsedMs.store(static_cast<float>(elapsedMs), std::memory_order_relaxed);

                        return result;
                    },
                    [recordPtr](bool succeeded) {
                        recordPtr->state.store(succeeded ? TaskState::Done : TaskState::Failed,
                                               std::memory_order_release);
                    });
            }
        }
    }

    bool InitTaskScheduler::AllDone() const
    {
        for (const TaskRecord& record : m_tasks) {
            const TaskState state = record.state.load(std::memory_order_acquire);
            if (state != TaskState::Done && state != TaskState::Failed) {
                return false;
            }
        }
        return true;
    }

    bool InitTaskScheduler::AnyFailed() const
    {
        for (const TaskRecord& record : m_tasks) {
            if (record.state.load(std::memory_order_acquire) == TaskState::Failed) {
                return true;
            }
        }
        return false;
    }

    uint32_t InitTaskScheduler::CompletedCount() const
    {
        uint32_t count = 0;
        for (const TaskRecord& record : m_tasks) {
            const TaskState state = record.state.load(std::memory_order_acquire);
            if (state == TaskState::Done || state == TaskState::Failed) {
                ++count;
            }
        }
        return count;
    }

    uint32_t InitTaskScheduler::TotalCount() const
    {
        return static_cast<uint32_t>(m_tasks.size());
    }

    void InitTaskScheduler::CollectStatus(BootStatus& out) const
    {
        for (const TaskRecord& record : m_tasks) {
            if (out.itemCount >= BootStatus::kMaxItems) {
                break;
            }

            BootStatus::Item& item = out.items[out.itemCount];
            strncpy_s(item.name, sizeof(item.name), record.name, _TRUNCATE);
            item.state = ToBootItemState(record.state.load(std::memory_order_acquire));
            item.elapsedMs = record.elapsedMs.load(std::memory_order_relaxed);

            ++out.itemCount;
        }

        out.completedCount = CompletedCount();
        out.totalCount = TotalCount();
        out.progress = (out.totalCount == 0u)
            ? 0.0f
            : static_cast<float>(out.completedCount) / static_cast<float>(out.totalCount);
    }

    void InitTaskScheduler::WaitForOutstandingWork()
    {
        // Main-thread tasks still queued in the MainThreadTaskPump (Pending/Dispatched
        // with mainThread == true) are intentionally left alone here: this method only
        // exists to safely shut down the JobSystem, and main-thread work has nowhere
        // else to run but the caller's own frame loop.
        for (TaskRecord& record : m_tasks) {
            if (record.mainThread) {
                continue;
            }

            const TaskState state = record.state.load(std::memory_order_acquire);
            if (state == TaskState::Dispatched || state == TaskState::Running) {
                JobSystem::Wait(record.counter);
            }
        }
    }
}
