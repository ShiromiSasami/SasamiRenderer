#pragma once

#include <cstdint>
#include <deque>
#include <functional>

namespace SasamiEngine::Foundation
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

// Lets the other layers name Foundation types unqualified. The directive sits on SasamiEngine rather
// than on a layer, so anything nested under it -- SasamiEngine::Render, SasamiEngine::App -- finds these
// names by enclosing-namespace lookup.
//
// The SasamiRenderer -> SasamiEngine::<layer> namespace migration it was written for is finished; what
// is left is deciding whether the layers should qualify Foundation:: explicitly instead. That is not a
// mechanical change (roughly 76 files name these types unqualified), so the directive stays until then.
namespace SasamiEngine { using namespace Foundation; }
