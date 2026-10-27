#include "AppFramework/Debug/RemoteControl/DebugCaptureCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/FrameRecorder.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <cstdlib>

#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    // Capture is split into a "request" and a "poll" command instead of one blocking call
    // because the handler runs on the main thread, and the capture can only complete during
    // a later frame on that same thread -- a handler that waited for the result would
    // deadlock the render loop. The caller polls instead (Build/screenshot.ps1 does this).
    void RegisterCaptureCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("debug.screenshot", "<path> - queue a back-buffer capture; poll debug.screenshot.status",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "ERR usage: debug.screenshot <path>";
                }
                if (app.GetFrameRecorder().IsRecording()) {
                    return "ERR debug.record is running; stop it first";
                }
                const std::string path = JoinFrom(args, 0);
                std::string resolved;
                if (!app.RequestScreenshot(path, resolved)) {
                    return "ERR could not queue capture";
                }
                return "OK queued " + resolved;
            });

        registry.Register("debug.screenshot.status", "poll the queued capture; OK <path> when written, PENDING while in flight",
            [&app](const std::vector<std::string>&) -> std::string {
                // The recorder owns the capture slot while it runs; polling here would steal its result.
                if (app.GetFrameRecorder().IsRecording()) {
                    return "ERR debug.record is running";
                }
                std::string message;
                if (!app.PollScreenshotResult(message)) {
                    return "PENDING";
                }
                return message;
            });

        registry.Register("debug.dumptarget",
            "<name> <path> - queue a dump of one render target to a PNG. name: backbuffer | shadow0..shadow3 | gbuffer.albedo | gbuffer.normal | gbuffer.material | gbuffer.emissive | gbuffer.specular. Poll debug.screenshot.status for completion.",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() != 2) {
                    return "ERR usage: debug.dumptarget <name> <path>";
                }
                if (app.GetFrameRecorder().IsRecording()) {
                    return "ERR debug.record is running; stop it first";
                }
                const std::string message = app.RequestTargetDump(args[0], args[1]);
                if (!message.empty()) {
                    return "ERR " + message;
                }
                return "OK queued " + args[1];
            });

        registry.Register("debug.record",
            "start <dir> [fps=30] [frames=N] [ui=0|1] | stop | status - write every frame's back buffer to <dir>/frame_%06d.png; the frame delta is fixed to 1/fps while recording",
            [&app](const std::vector<std::string>& args) -> std::string {
                FrameRecorder& recorder = app.GetFrameRecorder();
                if (args.empty() || args[0] == "status") {
                    return "OK " + recorder.Status();
                }
                if (args[0] == "stop") {
                    return recorder.Stop(app);
                }
                if (args[0] != "start" || args.size() < 2) {
                    return "ERR usage: debug.record start <dir> [fps=30] [frames=N] [ui=0|1] | stop | status";
                }
                FrameRecorderSettings settings;
                settings.directory = args[1];
                for (size_t i = 2; i < args.size(); ++i) {
                    const std::string& arg = args[i];
                    const size_t eq = arg.find('=');
                    if (eq == std::string::npos) {
                        return "ERR expected key=value, got " + arg;
                    }
                    const std::string key = arg.substr(0, eq);
                    const char* value = arg.c_str() + eq + 1;
                    char* end = nullptr;
                    if (key == "fps") {
                        settings.fps = std::strtof(value, &end);
                    } else if (key == "frames") {
                        settings.maxFrames = static_cast<uint32_t>(std::strtoul(value, &end, 10));
                    } else if (key == "ui") {
                        settings.hideUi = std::strtol(value, &end, 10) == 0;
                    } else {
                        return "ERR unknown key " + key;
                    }
                    if (end == value || *end != '\0') {
                        return "ERR bad value in " + arg;
                    }
                }
                return recorder.Start(app, settings);
            });
    }
}
