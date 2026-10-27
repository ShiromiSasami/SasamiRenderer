#pragma once

#include <cstdint>
#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        struct FrameRecorderSettings
        {
            std::string directory;      // UTF-8; created if missing, made absolute at Start
            float fps = 30.0f;          // also the fixed frame delta while recording (1/fps)
            uint32_t maxFrames = 0;     // 0 = until Stop
            bool hideUi = false;        // hide ImGui for the recording, restored at Stop
        };

        // Debug recorder (Discord request, 2026-10-22): writes the back buffer of every frame to
        // <directory>/frame_000000.png, ... through the debug.screenshot path, so it works on every
        // backend that path supports. While recording the frame delta is fixed to 1/fps, so the
        // sequence plays back at real speed at that frame rate no matter how slow the PNG encode
        // makes the actual loop (each capture waits for the GPU). No video encoder: turn the
        // sequence into a video offline (Stop returns an ffmpeg line). debug.screenshot is refused
        // while recording, because both share the renderer's single capture slot. Main thread only.
        class FrameRecorder
        {
        public:
            // Returns "OK ..." or "ERR ...".
            std::string Start(ApplicationCore& app, const FrameRecorderSettings& settings);
            std::string Stop(ApplicationCore& app);
            std::string Status() const;
            bool IsRecording() const { return m_recording; }

            // Once per frame, before the frame is rendered: collects the previous capture and
            // queues the next one.
            void Tick(ApplicationCore& app);

        private:
            void CollectResult(ApplicationCore& app);

            bool m_recording = false;
            bool m_awaitingResult = false;
            FrameRecorderSettings m_settings;
            std::string m_directory;
            uint32_t m_requested = 0;
            uint32_t m_written = 0;
            uint32_t m_skipped = 0;
            uint32_t m_errors = 0;
            std::string m_lastError;
            uint64_t m_startMs = 0;
            uint64_t m_endMs = 0;
            float m_previousFixedDt = 0.0f;
            bool m_previousUiVisible = true;
        };
    }
}
namespace SasamiEngine { using namespace App; }
