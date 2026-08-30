#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "AppFramework/Debug/RemoteControl/DebugNamedPipeTransport.h"

#include <string>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer::Debug
{
    namespace
    {
        constexpr DWORD kPipeBufferSize = 4096;
        constexpr DWORD kReadChunkSize = 4096;

        // Minimal RAII wrapper for Win32 HANDLEs. Treats both nullptr and
        // INVALID_HANDLE_VALUE as "not owning a handle".
        class ScopedHandle
        {
        public:
            ScopedHandle() = default;
            explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
            ~ScopedHandle() { Reset(); }

            ScopedHandle(const ScopedHandle&) = delete;
            ScopedHandle& operator=(const ScopedHandle&) = delete;

            ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
            ScopedHandle& operator=(ScopedHandle&& other) noexcept
            {
                if (this != &other) {
                    Reset();
                    m_handle = other.m_handle;
                    other.m_handle = nullptr;
                }
                return *this;
            }

            HANDLE Get() const { return m_handle; }
            bool IsValid() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
            explicit operator bool() const { return IsValid(); }

            void Reset(HANDLE handle = nullptr)
            {
                if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
                    ::CloseHandle(m_handle);
                }
                m_handle = handle;
            }

        private:
            HANDLE m_handle = nullptr;
        };

        enum class WaitOutcome { Completed, StopRequested, Failed };

        // Waits for a pending overlapped I/O operation to complete, or for
        // stopEvent to be signaled, whichever happens first. On StopRequested
        // the caller owns canceling the pending I/O via CancelIoEx.
        WaitOutcome WaitForOverlapped(HANDLE fileHandle, OVERLAPPED& overlapped, HANDLE stopEvent, DWORD& transferredBytes)
        {
            HANDLE waitHandles[2] = { overlapped.hEvent, stopEvent };
            const DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0 + 1) {
                return WaitOutcome::StopRequested;
            }
            if (waitResult != WAIT_OBJECT_0) {
                return WaitOutcome::Failed;
            }
            if (!::GetOverlappedResult(fileHandle, &overlapped, &transferredBytes, FALSE)) {
                return WaitOutcome::Failed;
            }
            return WaitOutcome::Completed;
        }
    }

    DebugNamedPipeTransport::DebugNamedPipeTransport(std::string pipeName)
        : m_pipeName(std::move(pipeName))
    {
    }

    DebugNamedPipeTransport::~DebugNamedPipeTransport()
    {
        Stop();
    }

    bool DebugNamedPipeTransport::Start(DebugLineHandler handler)
    {
        if (!handler) {
            return false;
        }

        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) {
            return false;
        }

        const HANDLE stopEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent == nullptr) {
            m_running.store(false);
            return false;
        }
        m_stopEvent = stopEvent;
        m_handler = std::move(handler);

        try {
            m_thread = std::thread(&DebugNamedPipeTransport::ThreadMain, this);
        } catch (...) {
            ::CloseHandle(stopEvent);
            m_stopEvent = nullptr;
            m_handler = nullptr;
            m_running.store(false);
            return false;
        }

        return true;
    }

    void DebugNamedPipeTransport::Stop()
    {
        if (!m_running.exchange(false)) {
            return;
        }

        const HANDLE stopEvent = static_cast<HANDLE>(m_stopEvent);
        if (stopEvent != nullptr) {
            ::SetEvent(stopEvent);
        }

        if (m_thread.joinable()) {
            m_thread.join();
        }

        if (stopEvent != nullptr) {
            ::CloseHandle(stopEvent);
        }
        m_stopEvent = nullptr;
        m_handler = nullptr;
    }

    const std::string& DebugNamedPipeTransport::EndpointName() const
    {
        return m_pipeName;
    }

    void DebugNamedPipeTransport::ThreadMain()
    {
        const HANDLE stopEvent = static_cast<HANDLE>(m_stopEvent);

        while (m_running.load()) {
            ScopedHandle pipe(::CreateNamedPipeA(
                m_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                kPipeBufferSize,
                kPipeBufferSize,
                0,
                nullptr));
            if (!pipe) {
                SasamiRenderer::DebugLog("[DebugNamedPipeTransport] Failed to create named pipe instance.");
                break;
            }

            ScopedHandle connectEvent(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
            if (!connectEvent) {
                break;
            }

            OVERLAPPED connectOverlapped{};
            connectOverlapped.hEvent = connectEvent.Get();

            const BOOL connectedImmediately = ::ConnectNamedPipe(pipe.Get(), &connectOverlapped);
            const DWORD connectError = ::GetLastError();

            bool connected = false;
            if (connectedImmediately) {
                connected = true;
            } else if (connectError == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (connectError == ERROR_IO_PENDING) {
                DWORD transferred = 0;
                const WaitOutcome outcome = WaitForOverlapped(pipe.Get(), connectOverlapped, stopEvent, transferred);
                if (outcome == WaitOutcome::StopRequested) {
                    ::CancelIoEx(pipe.Get(), &connectOverlapped);
                    break;
                }
                connected = (outcome == WaitOutcome::Completed);
            }

            if (!connected) {
                continue;
            }

            std::string readBuffer;
            bool clientConnected = true;
            bool stopRequested = false;

            while (clientConnected && m_running.load()) {
                char chunk[kReadChunkSize];
                ScopedHandle readEvent(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
                if (!readEvent) {
                    clientConnected = false;
                    break;
                }

                OVERLAPPED readOverlapped{};
                readOverlapped.hEvent = readEvent.Get();

                const BOOL readImmediate = ::ReadFile(pipe.Get(), chunk, sizeof(chunk), nullptr, &readOverlapped);
                const DWORD readError = ::GetLastError();
                if (!readImmediate && readError != ERROR_IO_PENDING) {
                    clientConnected = false;
                    break;
                }

                DWORD bytesRead = 0;
                const WaitOutcome readOutcome = WaitForOverlapped(pipe.Get(), readOverlapped, stopEvent, bytesRead);
                if (readOutcome == WaitOutcome::StopRequested) {
                    ::CancelIoEx(pipe.Get(), &readOverlapped);
                    stopRequested = true;
                    clientConnected = false;
                    break;
                }
                if (readOutcome != WaitOutcome::Completed || bytesRead == 0) {
                    clientConnected = false;
                    break;
                }

                readBuffer.append(chunk, bytesRead);

                size_t newlinePos = 0;
                while (clientConnected && (newlinePos = readBuffer.find('\n')) != std::string::npos) {
                    std::string line = readBuffer.substr(0, newlinePos);
                    readBuffer.erase(0, newlinePos + 1);

                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (line.empty()) {
                        continue;
                    }

                    std::string response = m_handler(line);
                    response.push_back('\n');

                    size_t written = 0;
                    while (written < response.size()) {
                        ScopedHandle writeEvent(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
                        if (!writeEvent) {
                            clientConnected = false;
                            break;
                        }

                        OVERLAPPED writeOverlapped{};
                        writeOverlapped.hEvent = writeEvent.Get();

                        const DWORD remaining = static_cast<DWORD>(response.size() - written);
                        const BOOL writeImmediate = ::WriteFile(
                            pipe.Get(), response.data() + written, remaining, nullptr, &writeOverlapped);
                        const DWORD writeError = ::GetLastError();
                        if (!writeImmediate && writeError != ERROR_IO_PENDING) {
                            clientConnected = false;
                            break;
                        }

                        DWORD bytesWritten = 0;
                        const WaitOutcome writeOutcome =
                            WaitForOverlapped(pipe.Get(), writeOverlapped, stopEvent, bytesWritten);
                        if (writeOutcome == WaitOutcome::StopRequested) {
                            ::CancelIoEx(pipe.Get(), &writeOverlapped);
                            stopRequested = true;
                            clientConnected = false;
                            break;
                        }
                        if (writeOutcome != WaitOutcome::Completed || bytesWritten == 0) {
                            clientConnected = false;
                            break;
                        }
                        written += bytesWritten;
                    }
                }
            }

            ::DisconnectNamedPipe(pipe.Get());

            if (stopRequested) {
                break;
            }
        }
    }
}
