#include "DebugRemoteControlServer.h"

#include <utility>

namespace SasamiRenderer::Debug
{
    DebugRemoteControlServer::~DebugRemoteControlServer()
    {
        Stop();
    }

    bool DebugRemoteControlServer::Start(std::unique_ptr<IDebugTransport> transport,
                                          std::chrono::milliseconds commandTimeout)
    {
        if (m_running || !transport)
        {
            return false;
        }

        m_commandTimeout = commandTimeout;

        m_registry.Register("help", "Lists every registered command.",
            [this](const std::vector<std::string>&) -> std::string
            {
                return m_registry.BuildHelpText();
            });

        // Runs on the transport's own receive thread; must not touch the
        // registry directly since engine state may only be accessed from the
        // main thread. Hand the line off to the queue instead.
        DebugLineHandler handler = [this](std::string_view requestLine) -> std::string
        {
            return m_queue.Submit(std::string(requestLine), m_commandTimeout);
        };

        if (!transport->Start(std::move(handler)))
        {
            return false;
        }

        m_transport = std::move(transport);
        m_running = true;
        return true;
    }

    void DebugRemoteControlServer::Stop()
    {
        // Release waiters BEFORE joining the transport. IDebugTransport::Stop() joins its
        // thread, and that thread may be parked inside DebugCommandQueue::Submit waiting on
        // a command that only DrainPendingCommands() could fulfil -- which cannot happen,
        // because the main thread is right here. Joining first would therefore stall for the
        // full command timeout. Shutting the queue down first releases that wait at once, and
        // any request arriving afterwards is answered immediately with "ERR server shutting
        // down" rather than hanging, so nothing is left dangling.
        m_queue.Shutdown();

        if (m_transport)
        {
            m_transport->Stop();
            m_transport.reset();
        }
        m_running = false;
    }

    // Main-thread only: executes commands submitted by the transport's
    // receive thread. Must be called once per frame.
    void DebugRemoteControlServer::DrainPendingCommands()
    {
        m_queue.Drain([this](std::string_view requestLine)
            {
                return m_registry.Execute(requestLine);
            });
    }

    DebugCommandRegistry& DebugRemoteControlServer::Registry()
    {
        return m_registry;
    }

    const DebugCommandRegistry& DebugRemoteControlServer::Registry() const
    {
        return m_registry;
    }

    bool DebugRemoteControlServer::IsRunning() const
    {
        return m_running;
    }
}
