#pragma once

#include <cstdint>
#include <deque>
#include <functional>

namespace SasamiRenderer
{
    // Result of one resumable step of a main-thread init task.
    enum class InitStepResult : uint8_t
    {
        Continue, // more work remains; call step() again (possibly next frame)
        Done,
        Failed
    };

    // Time-sliced executor for initialization work that must run on the main thread
    // (single-context backends like OpenGL, GPU queue submissions, window-coupled setup).
    // Tasks expose a resumable step() so one long task cannot starve the message pump:
    // Execute() keeps stepping the front task until the time budget is spent or the
    // task finishes, then yields back to the caller's frame loop.
    class MainThreadTaskPump
    {
    public:
        using StepFn = std::function<InitStepResult()>;
        using FinishedFn = std::function<void(bool succeeded)>;

        void Enqueue(StepFn step, FinishedFn onFinished);

        // Runs queued tasks front-to-back until budgetMs is exhausted or the queue
        // empties. Must be called from the main thread.
        void Execute(double budgetMs);

        bool Empty() const { return m_tasks.empty(); }

    private:
        struct Entry
        {
            StepFn step;
            FinishedFn onFinished;
        };

        std::deque<Entry> m_tasks;
    };
}
