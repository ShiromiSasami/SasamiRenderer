#include "Debug/RemoteControl/DebugCommandQueue.h"

namespace SasamiRenderer::Debug
{
    std::string DebugCommandQueue::Submit(std::string requestLine, std::chrono::milliseconds timeout)
    {
        std::future<std::string> future;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shuttingDown) {
                return "ERR server shutting down";
            }

            PendingCommand command;
            command.line = std::move(requestLine);
            future = command.result.get_future();
            m_pending.push_back(std::move(command));
        }

        if (future.wait_for(timeout) != std::future_status::ready) {
            return "ERR timeout";
        }

        try {
            return future.get();
        } catch (...) {
            return "ERR internal error";
        }
    }

    void DebugCommandQueue::Drain(const std::function<std::string(std::string_view)>& executor)
    {
        std::deque<PendingCommand> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending.empty()) {
                return;
            }
            std::swap(pending, m_pending);
        }

        for (auto& command : pending) {
            std::string response;
            try {
                response = executor(command.line);
            } catch (...) {
                response = "ERR executor threw an exception";
            }
            command.result.set_value(std::move(response));
        }
    }

    void DebugCommandQueue::Shutdown()
    {
        std::deque<PendingCommand> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shuttingDown = true;
            std::swap(pending, m_pending);
        }

        for (auto& command : pending) {
            command.result.set_value("ERR server shutting down");
        }
    }
}
