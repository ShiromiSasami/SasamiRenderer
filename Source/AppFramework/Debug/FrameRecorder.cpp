#include "AppFramework/Debug/FrameRecorder.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/UI/ImGuiCoordinator.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace SasamiEngine::App::Debug
{
    std::string FrameRecorder::Start(ApplicationCore& app, const FrameRecorderSettings& settings)
    {
        if (m_recording) {
            return "ERR already recording to " + m_directory;
        }
        if (m_awaitingResult) {
            return "ERR previous recording still has a capture in flight; retry next frame";
        }
        if (settings.directory.empty()) {
            return "ERR empty directory";
        }
        if (!(settings.fps > 0.0f && settings.fps <= 240.0f)) {
            return "ERR fps must be in (0, 240]";
        }

        std::error_code ec;
        std::filesystem::path dir = std::filesystem::path(settings.directory);
        std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
        if (!ec) {
            dir = absolute;
        }
        std::filesystem::create_directories(dir, ec);
        if (ec || !std::filesystem::is_directory(dir)) {
            return "ERR could not create " + dir.string();
        }

        m_settings = settings;
        m_directory = dir.string();
        m_requested = 0;
        m_written = 0;
        m_skipped = 0;
        m_errors = 0;
        m_lastError.clear();
        m_startMs = GetTickCount64();
        m_endMs = 0;

        m_previousFixedDt = app.GetFixedDeltaTime();
        app.SetFixedDeltaTime(1.0f / settings.fps);
        ImGuiCoordinator& ui = ImGuiCoordinator::Instance();
        m_previousUiVisible = ui.IsDrawVisible();
        if (settings.hideUi) {
            ui.SetDrawVisible(false);
        }

        m_recording = true;
        return "OK recording dir=" + m_directory + " fps=" + std::to_string(settings.fps) +
               " maxFrames=" + std::to_string(settings.maxFrames);
    }

    std::string FrameRecorder::Stop(ApplicationCore& app)
    {
        if (!m_recording) {
            return "OK not recording (" + Status() + ")";
        }
        m_recording = false;
        m_endMs = GetTickCount64();
        app.SetFixedDeltaTime(m_previousFixedDt);
        if (m_settings.hideUi) {
            ImGuiCoordinator::Instance().SetDrawVisible(m_previousUiVisible);
        }

        char fps[32];
        std::snprintf(fps, sizeof(fps), "%g", m_settings.fps);
        // A capture still in flight is collected by Tick, so it never leaks into a later
        // debug.screenshot.status poll; it is counted in `requested` already.
        return "OK stopped " + Status() + " | ffmpeg -framerate " + fps +
               " -i \"" + m_directory + "\\frame_%06d.png\" -vf \"crop=trunc(iw/2)*2:trunc(ih/2)*2\" -c:v libx264 -pix_fmt yuv420p out.mp4";
    }

    std::string FrameRecorder::Status() const
    {
        const uint64_t end = m_recording ? GetTickCount64() : m_endMs;
        const double seconds = (m_startMs != 0 && end > m_startMs) ? static_cast<double>(end - m_startMs) * 0.001 : 0.0;
        const double captureFps = seconds > 0.0 ? static_cast<double>(m_written) / seconds : 0.0;
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "recording=%d written=%u requested=%u skipped=%u errors=%u wallSec=%.2f captureFps=%.1f",
                      m_recording ? 1 : 0, m_written, m_requested, m_skipped, m_errors, seconds, captureFps);
        std::string status = buffer;
        if (!m_directory.empty()) {
            status += " dir=" + m_directory;
        }
        if (!m_lastError.empty()) {
            status += " lastError=" + m_lastError;
        }
        return status;
    }

    void FrameRecorder::CollectResult(ApplicationCore& app)
    {
        if (!m_awaitingResult) {
            return;
        }
        std::string message;
        if (!app.PollScreenshotResult(message)) {
            return;
        }
        m_awaitingResult = false;
        if (message.rfind("OK", 0) == 0) {
            ++m_written;
        } else {
            ++m_errors;
            m_lastError = message;
        }
    }

    void FrameRecorder::Tick(ApplicationCore& app)
    {
        CollectResult(app);
        if (!m_recording) {
            return;
        }
        if (m_settings.maxFrames > 0 && m_requested >= m_settings.maxFrames) {
            if (!m_awaitingResult) {
                Stop(app);
            }
            return;
        }
        if (m_awaitingResult) {
            // The capture is resolved at the end of the frame that recorded it, so this only
            // happens if that frame never presented.
            ++m_skipped;
            return;
        }

        char name[32];
        std::snprintf(name, sizeof(name), "frame_%06u.png", m_requested);
        const std::string path = (std::filesystem::path(m_directory) / name).string();
        std::string resolved;
        if (!app.RequestScreenshot(path, resolved)) {
            ++m_skipped;
            return;
        }
        m_awaitingResult = true;
        ++m_requested;
    }
}
