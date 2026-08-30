#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "AppFramework/Debug/RemoteControl/IDebugTransport.h"

namespace SasamiRenderer::Debug
{
    // Windows named-pipe implementation of IDebugTransport. Runs a dedicated
    // worker thread that accepts one client connection at a time, reads
    // newline-terminated request lines, and writes back the handler's
    // newline-terminated response.
    class DebugNamedPipeTransport final : public IDebugTransport
    {
    public:
        explicit DebugNamedPipeTransport(std::string pipeName = R"(\\.\pipe\SasamiRenderer.Debug)");
        ~DebugNamedPipeTransport() override;

        DebugNamedPipeTransport(const DebugNamedPipeTransport&) = delete;
        DebugNamedPipeTransport& operator=(const DebugNamedPipeTransport&) = delete;

        bool Start(DebugLineHandler handler) override;
        void Stop() override;

        const std::string& EndpointName() const override;

    private:
        void ThreadMain();

        std::string m_pipeName;
        DebugLineHandler m_handler;
        std::thread m_thread;
        std::atomic<bool> m_running { false };
        void* m_stopEvent = nullptr;
    };
}
