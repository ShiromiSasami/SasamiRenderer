#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace SasamiRenderer::Debug
{
    // Handles one request line and returns the line to send back. Invoked on the
    // transport's own thread; implementations of this callback are responsible for
    // getting the work onto whichever thread may legally touch engine state.
    using DebugLineHandler = std::function<std::string(std::string_view requestLine)>;

    // A line-oriented, local-only request/response channel for the debug remote control.
    //
    // This is the ONLY place the remote-control stack knows how bytes travel. Named pipes
    // ship first (no port to collide with, no Windows Firewall prompt, ACLs give access
    // control for free); a TCP implementation can be added later without touching the
    // queue, registry, command or server layers.
    class IDebugTransport
    {
    public:
        virtual ~IDebugTransport() = default;

        // Begins listening. Returns false if the endpoint could not be opened.
        virtual bool Start(DebugLineHandler handler) = 0;

        // Stops listening and joins the transport thread. Safe to call when not started.
        virtual void Stop() = 0;

        // Human-readable endpoint, for logging (e.g. the pipe name).
        virtual const std::string& EndpointName() const = 0;
    };
}
