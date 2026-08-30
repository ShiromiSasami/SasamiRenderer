#pragma once

#include <chrono>
#include <memory>

#include "DebugCommandQueue.h"
#include "DebugCommandRegistry.h"
#include "IDebugTransport.h"

namespace SasamiRenderer::Debug
{
    // Top-level façade that owns the transport, the hand-off queue and the
    // command registry for the debug remote control feature. This is the only
    // class the application is expected to talk to directly.
    class DebugRemoteControlServer
    {
    public:
        DebugRemoteControlServer() = default;
        ~DebugRemoteControlServer();

        DebugRemoteControlServer(const DebugRemoteControlServer&) = delete;
        DebugRemoteControlServer& operator=(const DebugRemoteControlServer&) = delete;
        DebugRemoteControlServer(DebugRemoteControlServer&&) = delete;
        DebugRemoteControlServer& operator=(DebugRemoteControlServer&&) = delete;

        // Takes ownership of transport and begins listening. Returns false if
        // already started, transport is null, or transport->Start fails.
        bool Start(std::unique_ptr<IDebugTransport> transport,
                   std::chrono::milliseconds commandTimeout = std::chrono::milliseconds(5000));

        // Stops listening and releases any commands still waiting on the queue.
        // Safe to call when not started; also called from the destructor.
        void Stop();

        // Must be called once per frame on the main thread. Executes any
        // commands that have been submitted by the transport's receive thread.
        void DrainPendingCommands();

        // For registering commands. May be called before or after Start.
        DebugCommandRegistry& Registry();
        const DebugCommandRegistry& Registry() const;

        bool IsRunning() const;

    private:
        std::unique_ptr<IDebugTransport> m_transport;
        DebugCommandQueue m_queue;
        DebugCommandRegistry m_registry;
        std::chrono::milliseconds m_commandTimeout = std::chrono::milliseconds(5000);
        bool m_running = false;
    };
}
