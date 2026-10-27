#include "AppFramework/Debug/RemoteControl/DebugAppCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/UI/ImGuiCoordinator.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    void RegisterAppCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("app.fixeddt", "[seconds|off] - get/set a fixed frame delta (0/off = wall clock)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    if (args[0] == "off") {
                        app.SetFixedDeltaTime(0.0f);
                    } else {
                        char* end = nullptr;
                        const float seconds = std::strtof(args[0].c_str(), &end);
                        if (end == args[0].c_str() || *end != '\0' || seconds < 0.0f) {
                            return "ERR usage: app.fixeddt <seconds|off>";
                        }
                        app.SetFixedDeltaTime(seconds);
                    }
                }
                return "OK fixedDt=" + std::to_string(app.GetFixedDeltaTime());
            });

        registry.Register("app.ui", "[show|hide] - get/set whether the UI is drawn into the image",
            [](const std::vector<std::string>& args) -> std::string {
                ImGuiCoordinator& ui = ImGuiCoordinator::Instance();
                if (!args.empty()) {
                    if (args[0] == "show") {
                        ui.SetDrawVisible(true);
                    } else if (args[0] == "hide") {
                        ui.SetDrawVisible(false);
                    } else {
                        return "ERR usage: app.ui <show|hide>";
                    }
                }
                return std::string("OK ui=") + (ui.IsDrawVisible() ? "show" : "hide");
            });

        // Polled by Tools/Debug/golden.ps1 to settle a view for N frames instead of sleeping.
        registry.Register("app.frame", "scene frames rendered since boot handed off",
            [&app](const std::vector<std::string>&) -> std::string {
                return "OK frame=" + std::to_string(app.GetSceneFrameCount());
            });

        // render.get answers as soon as the scene exists, but deferred boot tasks (IBL, GI,
        // BVH) are still landing then; captures must wait for Running.
        registry.Register("app.ready", "1 once boot reached the Running phase",
            [&app](const std::vector<std::string>&) -> std::string {
                return std::string("OK ready=") + (app.IsBootRunning() ? "1" : "0");
            });
    }
}
