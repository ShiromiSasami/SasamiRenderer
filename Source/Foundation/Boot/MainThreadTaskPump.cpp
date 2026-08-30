#include "Foundation/Boot/MainThreadTaskPump.h"

#include <chrono>

namespace SasamiRenderer
{
    void MainThreadTaskPump::Enqueue(StepFn step, FinishedFn onFinished)
    {
        m_tasks.push_back(Entry{ std::move(step), std::move(onFinished) });
    }

    void MainThreadTaskPump::Execute(double budgetMs)
    {
        const auto startTime = std::chrono::steady_clock::now();

        bool ranOne = false;
        while (!m_tasks.empty()) {
            if (ranOne) {
                const auto now = std::chrono::steady_clock::now();
                const double elapsedMs = std::chrono::duration<double, std::milli>(now - startTime).count();
                if (elapsedMs >= budgetMs) {
                    break;
                }
            }

            Entry& front = m_tasks.front();
            const InitStepResult result = front.step();
            ranOne = true;

            if (result == InitStepResult::Continue) {
                continue;
            }

            if (front.onFinished) {
                front.onFinished(result == InitStepResult::Done);
            }
            m_tasks.pop_front();
        }
    }
}
