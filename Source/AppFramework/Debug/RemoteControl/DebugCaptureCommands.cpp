#include "AppFramework/Debug/RemoteControl/DebugCaptureCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"

#include <string>
#include <vector>

namespace SasamiRenderer::Debug
{
    namespace
    {
        // Joins the remaining tokens back with single spaces, so a path may contain spaces
        // even though the registry splits purely on whitespace.
        std::string JoinFrom(const std::vector<std::string>& args, size_t first)
        {
            std::string joined;
            for (size_t i = first; i < args.size(); ++i) {
                if (!joined.empty()) {
                    joined += ' ';
                }
                joined += args[i];
            }
            return joined;
        }
    }

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
                const std::string path = JoinFrom(args, 0);
                std::string resolved;
                if (!app.RequestScreenshot(path, resolved)) {
                    return "ERR could not queue capture";
                }
                return "OK queued " + resolved;
            });

        registry.Register("debug.screenshot.status", "poll the queued capture; OK <path> when written, PENDING while in flight",
            [&app](const std::vector<std::string>&) -> std::string {
                std::string message;
                if (!app.PollScreenshotResult(message)) {
                    return "PENDING";
                }
                return message;
            });
    }
}
