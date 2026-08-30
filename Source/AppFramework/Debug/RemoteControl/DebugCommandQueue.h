#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>

namespace SasamiRenderer::Debug
{
    // Thread-safe hand-off queue between the debug transport's receive thread
    // and the main thread. The receive thread submits a request line and
    // blocks until the main thread drains the queue and produces a response
    // (or the wait times out / the queue is shut down).
    class DebugCommandQueue
    {
    public:
        DebugCommandQueue() = default;
        ~DebugCommandQueue() = default;

        DebugCommandQueue(const DebugCommandQueue&) = delete;
        DebugCommandQueue& operator=(const DebugCommandQueue&) = delete;

        // Called from the receive thread. Blocks until the main thread executes
        // the command and returns its result, the timeout elapses, or the queue
        // is shut down. Never throws; timeouts and shutdown are reported as a
        // single-line "ERR ..." string.
        std::string Submit(std::string requestLine, std::chrono::milliseconds timeout);

        // Called from the main thread once per frame. Runs every pending
        // command through executor and hands the result back to whichever
        // Submit call is waiting for it. Does nothing if the queue is empty.
        void Drain(const std::function<std::string(std::string_view)>& executor);

        // Called on shutdown. Releases every Submit call currently waiting
        // with an error, and causes all future Submit calls to fail
        // immediately.
        void Shutdown();

    private:
        struct PendingCommand
        {
            std::string line;
            std::promise<std::string> result;
        };

        std::mutex m_mutex;
        std::deque<PendingCommand> m_pending;
        bool m_shuttingDown = false;
    };
}
